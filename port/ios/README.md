# Build and install Halo: CE on iPhone and iPad

This directory builds a native ARM64 iOS app around the existing game's ILP32
runtime. It uses SDL3, OpenGL ES 3, UIKit touch controls, and the user's original
Xbox map files. The game files are separate from the application and are never
included in source control.

The Home Screen name is **Halo: CE**. The portable ILP32 runtime lives in
`port/runtime`; UIKit, Darwin, touch, audio, and the native loader live here.
The existing Android, Linux, Windows and web build targets are retained.

The icon is original geometric artwork; see [icon notes](ICON.md). Apple builds
exclude the bundled replacement HUD/title/font assets and require a user-owned
disc image. See [distribution status](../apple/DISTRIBUTION.md).

## Build

Use an Apple Silicon Mac with Xcode, its iOS SDK, Python 3, CMake, Ninja, LLVM
and LLD. The validated toolchain is Xcode 27 and Homebrew LLVM/LLD 23.1.2.
The deployment target is iOS 16, but older OS versions have not been validated.
Install dependencies using `brew install cmake ninja llvm lld sdl3 pkgconf`.
Open Xcode once to accept its license and install the iOS platform. Ensure
`xcode-select -p` points into full Xcode, not only Command Line Tools.
For signed device builds, add your Apple account in Xcode Settings > Accounts.
Your provisioning profile must cover the device and chosen bundle identifier.
See [Apple's device setup guide](https://developer.apple.com/documentation/xcode/running-your-app-on-simulated-or-physical-devices).

From the repository root:

```sh
# Native ARM64 simulator app (no signing).
python3 tools/ios_build.py --simulator

# Device app, signed with your Apple development team.
python3 tools/ios_build.py --team YOUR_TEAM_ID --bundle-id com.yourname.haloce

# Unsigned device IPA (no Apple account needed to build).
python3 tools/ios_build.py --unsigned --ipa dist/Halo-CE-iOS-unsigned.ipa
```

The script fetches pinned Khronos headers, SDL release-3.4.16 and musl 1.2.5,
compiles the guest, embeds it into signed application text, and builds the host.
Specify `--llvm` and `--lld` for non-default toolchain locations.
Game assertions remain enabled. An Xcode `Release` host configuration does not
disable the game's assertions. PGO and LTO are disabled for this initial port.

Outputs:

- `build/ios/app-device/Release-iphoneos/HaloCE.app`
- `build/ios/app-simulator/Release-iphonesimulator/HaloCE.app`
- `build/ios/app-unsigned/Release-iphoneos/HaloCE.app`
- `dist/Halo-CE-iOS-unsigned.ipa` and its `.sha256` checksum when requested

The default bundle identifier is `org.haloce.ios`. Set `--bundle-id` to one
covered by your signing profile. `--ipa PATH` can also package a signed build;
only unsigned builds are suitable for this project's public release workflow.
Code signing and provisioning must succeed
before installation. No jailbreak, JIT entitlement, or writable executable
memory is used.

## Simulator smoke tests

After installing the simulator app and importing an ISO through the app,
these explicit launch settings bypass the room chooser for repeatable tests:

```sh
xcrun simctl launch SIMULATOR_UDID org.haloce.ios
SIMCTL_CHILD_HALO_IOS_TEST_ROOM=YOUR_TEST_ROOM xcrun simctl launch SIMULATOR_UDID org.haloce.ios
```

Terminate the app between launches. These settings apply only to simulator
builds; normal launches open the game menus. `HALO_IOS_TEST_ROOM_UI=1`
opens the room form with its keyboard for simulator layout inspection. Use a unique room code
and the same code in a browser build from the pinned PR #12 source.

## Install and add game data

The cache validator accepts these exact Xbox v5 cache builds on iOS:

- PAL: `01.01.14.2342`
- NTSC-US: `01.10.12.2276` (experimental compatibility)

### Import on the device

1. Sign/install the IPA with your own account, then open **Halo: CE**.
2. Tap **Choose Halo XISO** and select your `.iso` or `.xiso` in Files (On My
   iPhone/iPad, iCloud Drive, or another Files provider). Compressed ZIP/7z
   archives and PC/MCC disc images are not supported.
3. The app copies the image into a private generation, verifies the copy with
   SHA-256 readback, and validates the Xbox filesystem, cache version/build,
   and complete campaign map set during extraction. Cancel safely stops it.
   Once finished, the game starts automatically.

The image is opened through the system document picker with coordinated,
security-scoped access. A cloud image may need to download before extraction.
Leave enough local storage for the retained image plus approximately 1.9 GB
of extracted maps, and any separate source copy already on the phone.
Nothing is fetched from a game-download service, and the source image is never
modified or deleted. Subsequent launches use the private image/map generation
without accessing the original Files location.

You can also copy **one** `.iso` or `.xiso` directly into Halo: CE's Documents
folder using Finder's Files tab or Files > On My iPhone/iPad > Halo: CE, then
launch the app. It detects and imports that image when game data is missing.
If several images are present, choose one with the picker. After a successful
import, the original loose source file can be removed. Keep the retained
`game-UUID/disc.iso` and its map cache; deleting these requires re-import.

Imports run in a private staging directory. Invalid or cancelled imports do
not replace the current game generation. Owned incomplete imports are cleaned
up on next launch. Existing committed generations are preserved. Saves
and profiles stay in `Documents/save` throughout.

### Install a source build

List devices with `xcrun devicectl list devices`. Enable Developer Mode when
iOS requests it, pair/trust the Mac, and keep the device unlocked during
installation. Replace `DEVICE_UDID` and the example bundle identifier with
your device and the ID used for signing.

```sh
xcrun devicectl device install app --device DEVICE_UDID \
  build/ios/app-device/Release-iphoneos/HaloCE.app
xcrun devicectl device process launch --device DEVICE_UDID com.yourname.haloce
```

Downloaded unsigned IPAs must first be signed with your own Apple account.
For packaged-IPA signing instructions, see the official
[AltStore Classic setup guide](https://faq.altstore.io/) or
[Sideloadly FAQ](https://sideloadly.io/faq). These signing tools have not been
validated as part of this port; direct Xcode builds have been tested on iPhone 13 Pro. Follow your signing tool's refresh instructions before
the provisioning profile expires. Keep the same account and bundle identifier
when updating to preserve the app's data.

### Optional manual extraction

The Mac helper remains available for inspecting your XISO or preparing maps
manually. It preserves original bytes and records build IDs, sizes and SHA-256
hashes. Existing output files are never replaced.

```sh
python3 tools/ios_extract_assets.py '/path/to/Halo.xiso.iso'
python3 tools/ios_extract_assets.py '/path/to/Halo.xiso.iso' --output assets
```

Manual map extraction is an inspection tool; maps alone no longer satisfy
first-launch setup. Import the image through the app to retain its verified
private copy. `config.toml` and `ios-runtime.log` stay in Documents; the game's
`debug.txt` is in the active `game-UUID` folder. Back up `Documents/save`
before uninstalling or changing bundle IDs.

## Controls

### Resolution

Native physical display resolution is the default, with a Retina drawable and
matching internal color/depth targets. The game retains its original logical
layout coordinates, so the HUD and touch controls keep their size.

For a lower GPU workload, edit the existing `[display]` section of
`Documents/config.toml` and relaunch:

```toml
render_height = 0 # Native (default); 1080, 720, or 480 render fewer pixels
screen_width = 0  # Fit the display; 640 selects original 4:3
```

The renderer preserves the selected aspect ratio and caps the render size to
the drawable and GPU texture limit. Existing configuration files without
`render_height` automatically use native resolution. Anti-aliasing and an
in-app graphics settings menu are not implemented yet.

### Touch and controller input

The left stick moves and the right stick aims. The four arrows navigate menus.
A selects/jumps; B returns/melees; X reloads/uses; Y changes weapons. Separate
buttons provide fire, grenade, crouch, zoom, flashlight, grenade selection,
and pause. Hold buttons for held actions. Touch controls hide automatically
when a hardware controller connects and return when it disconnects. There is
no hide/show toggle. The native Mac app never creates a touch overlay.
Compatible Backbone, Xbox and PlayStation controllers use SDL's iOS gamepad
backend. Pair Bluetooth controllers in iOS Settings > Bluetooth before or
while the app runs; attach a compatible wired Backbone directly to the phone.
See [Apple's controller pairing guide](https://support.apple.com/en-ie/111099).
The USB-C Backbone requires a compatible USB-C device; it cannot plug directly
into the Lightning iPhone 13 Pro.

The first hardware controller shares player one with the on-screen controls.
Touch controls and their visibility toggle hide when a gamepad connects and
return when it disconnects. Player one's controller
stays selected while connected, even if another controller is attached.

| Action | Xbox / standard labels | PlayStation labels |
| --- | --- | --- |
| Select / jump | A | Cross |
| Back / melee | B | Circle |
| Reload / use | X | Square |
| Switch weapon | Y | Triangle |
| Fire / grenade | RT / LT | R2 / L2 |
| Flashlight / switch grenade | RB / LB | R1 / L1 |
| Crouch / zoom | Left / right stick click | L3 / R3 |
| Pause | Menu / Start | Options |

The user confirmed an Xbox Series X controller works on the iPhone.
Backbone and PlayStation hardware validation remains pending;
controller model-specific capabilities such as haptics are not promised.
Developer console messages, frame counters, profiling text and the menu's build label are omitted
from the game picture. Diagnostic log files remain available in Documents.

The app supports landscape only on iPhone and iPad, including XISO import and
the Files picker. On iPadOS 26 and later it also requests the interface
orientation lock for the full-screen scene; portrait is excluded from the
app's orientation masks. iPadOS controls windowed multitasking and may
letterbox the landscape app when a full-screen orientation lock is unavailable.
The desktop UDP invite service and clipboard joining default to off on iOS;
browser rooms use the separate transport described below. Bink intro videos remain unsupported by the upstream port.

## Browser multiplayer (draft)

This integration is based on [PR #12](https://github.com/cybersecurity/halo-ce-universal/pull/12)
at `eaa82e6803f3d3e67c91d7f2fa2b15ee04e195f5`. Build the web client from that
revision (or this branch). Both use network version **11**. An older deployed
web demo may have another network version and is not a supported comparison.
Both players need matching original Xbox maps. Protocol compatibility does not
establish compatibility between different game-data releases.

After map import, the app opens Halo's main menu. Select **Multiplayer →
System Link** to open the native online-room form, then enter the web player's
room code and choose **Join room**. The original Xbox maps supply the System
Link label; on this iOS target it opens browser-compatible online rooms.
The keyboard stays closed until the code field is tapped. The form scrolls,
and Cancel/Join stay above the keyboard in landscape.
The shared room controller elects a host and starts quick play, initially
Beaver Creek Slayer. A native player can be elected host or join a web host.
Cancel returns to the menus. After leaving a match, reopen System Link to
choose another room. Keep the app in the foreground.
Background survival and recovery on physical devices remain unverified.

The game, renderer, audio and memory runtime are compiled native ARM64 code.
A retained `WKWebView` runs the **bundled, unchanged** `port/web/site/net.js`
for encrypted MQTT signaling, WebRTC channels, election and host migration.
There is no WASM game runtime in the iOS app. `network/room.js` exchanges bounded
packet batches with `network/room_bridge.c`; `web_net.c` implements the same
virtual sockets as the browser. Reliable streams and unreliable datagrams use
the same wire framing. Game-thread control imports carry hold, reconnect,
migration, cancellation, checkpoint reports and ping tables across the ILP32 ABI.

This WebKit dependency avoids maintaining a second version of the room protocol.
Packet batching and base64 copying add overhead that has not been profiled in
a full match. If WebKit terminates, the game is held and the app must be restarted.
The room view stays attached to the foreground scene below the game window.

The shared transport uses public signaling/STUN services. Networks requiring
TURN need the same TURN configuration as the web client. Optionally place this
in `Documents/browser-room.json` (use your own server and credentials):

```json
{"turn":{"urls":["turn:relay.example.com:3478"],"username":"player","credential":"secret"}}
```

This file stays outside the bundle and repository. The desktop native UDP
invite protocol is a different transport and is not bridged by this change.

### Provenance and scope

The signed ARM64 ILP32 runtime, memory layout, UIKit input, GLES renderer,
XISO importer, and build tooling are imported/adapted from
[NicholasDominici/halo-ce-ios](https://github.com/NicholasDominici/halo-ce-ios/tree/3f2c14101d3ae1c7f0c0a11993a43407fadbeb46),
branch `ios-port`. Integration changes keep PR #12's current game/network source,
use its musl math, preserve other platforms, and connect the Apple runtime to
browser rooms. Apple distribution builds omit the embedded HUD/title/font
replacements. This target builds iPhone/iPad apps; [port/macos](../macos/README.md)
builds the native Mac app. Dedicated visionOS and tvOS products remain open.

## How the port works

The game relies on 32-bit pointers in its data structures. Apple's current
ARM64 iOS binaries require their first 4 GB of address space to remain unmapped,
so the game's 32-bit pointers are represented as offsets into an aligned
native arena. This is native compiled code, not an Android emulator.

`tools/ios_asm_convert.py` lifts compiled guest memory accesses and indirect
branches into an aligned 4 GB arena using reserved registers x15 and x27.
Guest pointer values and structure layouts stay 32-bit. PC-relative addresses
are normalized back to guest offsets. A small signed assembly entry switches
to an arena stack; host bridges translate pointers at the ABI boundary.

Guest code is embedded in the app's signed `__TEXT` and aliased into the arena
read/execute with `vm_remap`. Guest data is separately writable. The host
translates Linux/musl calls to Darwin and handles Apple's 16 KB pages. Explicit
Xbox 4 KB read-only regions protect only complete native pages, preserving
writable neighboring buffers. Texture dirty tracking works at 16 KB granularity
and serializes protection changes against concurrent streaming writers.

The generated bridge resolves all guest imports before entering the game.
The portable runtime and assembly conversion tools were derived from the
upstream ARM64 port, with the iOS host and address model imported from NicholasDominici/halo-ce-ios.
SDL/UIKit calls stay on the UI thread; guest workers and audio callbacks run on
arena stacks with the same translation convention.
The audio worker stages mixed PCM for the SDL callback to submit on its own
thread. This avoids taking SDL's stream lock from a worker while the callback
holds that lock and waits for the worker.

## Regression checks

```sh
brew install sdl3 pkgconf  # Native macOS SDL library for the audio regression.
python3 tools/ios_test.py
```

This executes the translated guest on signed native pages and checks 32-bit
structure layout, global/stack access, function pointers, atomic operations,
and zero-extension of addresses. A separate stress test covers concurrent
texture page tracking, fresh zeroed mappings, and the map inflater's writable
4 KB tail beside a read-only buffer.
The SDL regression checks 100 callbacks and 134,144 exact PCM samples,
including reused guest stack buffers and a request larger than 64 KB.
The native XISO importer is tested under AddressSanitizer and UBSan with
synthetic disc images: exact byte preservation, supported builds, invalid
headers, missing maps, truncated extents, cycles, unsafe names, duplicate
files, cancellation/retry, existing files, and malformed directory mutations.

These tests do not replace a device campaign test. See [the validation record](VALIDATION.md)
for observed device/simulator behavior and remaining limitations.

## Simulator

Build with `--simulator`, then select and boot an ARM64 iPhone or iPad simulator
in Xcode. With exactly one simulator booted:

```sh
xcrun simctl install booted build/ios/app-simulator/Release-iphonesimulator/HaloCE.app
HALO_SIM_DATA=$(xcrun simctl get_app_container booted org.haloce.ios data)
cp "/path/to/your/game.xiso" "$HALO_SIM_DATA/Documents/game.xiso"
xcrun simctl launch booted org.haloce.ios
```

Use an explicit simulator ID instead of `booted` when more than one is running.
The app imports the single loose image on first launch. Simulator graphics
are slow; validate performance on a physical device.

## Troubleshooting

- **Signing fails:** check the Apple account in Xcode, team ID, unique bundle
  identifier, device registration, and profile expiration. The script permits
  Xcode provisioning updates; it never supplies somebody else's certificate.
- **App fails to launch:** confirm it was signed for your device, trust the
  developer if requested, and enable Developer Mode. An unsigned IPA will not launch.
- **Menu never appears or maps fail validation:** check `Documents/game-UUID/maps/ui.map`,
  confirm exact original Xbox build IDs above, and inspect `ios-runtime.log`
  and the active generation’s `debug.txt`. Re-import if the retained image or
  validated map cache is incomplete.
- **Changing your bundle ID:** iOS treats this as a separate app/container.
  Keep the same ID when updating and back up `Documents/save` before uninstalling.
- **Audio issues:** share device/OS and output route along with the runtime
  log. The regression suite checks PCM handoff; it does not test every route.

## Graphics debug and shader reports

Shake the iPhone/iPad to open **Graphics debug**. The UIKit main-menu builder
also registers **Debug → Graphics Debug…** (⇧⌘D) for the iPad app on an Apple
Silicon Mac. This branch does not yet contain a separate AppKit/macOS target;
that menu route has not been verified on a Mac.

- **Renderer** persists the choice between **Metal** (the default) and
  **OpenGL**. Metal uses ANGLE's Metal backend for game rendering; OpenGL
  uses Apple's OpenGL ES driver. Relaunch the app after changing renderers.
  Shader/texture/command translation still starts from the shared GLES API;
  this is not a separate handwritten Metal engine.
- **MetalFX spatial upscaling** is shown only when Metal is selected. It renders at up to 720 lines and upscales to the
  display resolution. It is opt-in, requires a supported physical device, and
  is unavailable in Apple's simulator SDK. ANGLE translates GLES calls and shaders to Metal. A shared
  IOSurface carries its image to MetalFX. Explicit synchronization adds cost,
  so enabling it is not a guarantee of higher frame rates. It is spatial, not
  temporal upscaling. Allocation or command-buffer failure disables upscaling and returns to
  normal Metal presentation. OpenGL mode never enables MetalFX.
- **Record shader stalls** records compile/link completion times and each
  program's first draw after recording starts, keeping events ≥ 2 ms and
  compile/link failures. First-use measurements drain earlier GPU work and
  wait for the draw, so recording adds overhead. They can reveal deferred
  compilation but also include drawing cost; they are not isolated compiler
  measurements and do not cover every driver pipeline variant.
- **Share shader report…** opens the system share sheet. Choose AirDrop and
  your Mac. Reports contain generated GLSL, source hashes/stages, program
  combinations, timings, device/OS/GPU and build metadata. They contain no
  game maps or saves. Nothing is sent until you choose a sharing destination.
  Capture retains up to 32 MiB of sources and 4096 events; dropped records are
  counted. The five latest exports are retained in Caches/ShaderReports.

The renderer and both switches persist across launches. For useful reports, enable recording,
restart the app, visit the affected level, then share the report. Turn recording
off again for normal play. Clear resets events and first-use tracking.
Reports provide inputs for future shader warm-up updates; this GLES renderer
currently has no bundled precompiled pipeline catalog. A report is not a
portable Metal binary archive, and cannot guarantee removal of all future
first-use stalls.

For automated diagnostics, launch environments `HALO_IOS_TEST_DEBUG_UI=1`,
`HALO_IOS_TEST_SHADER_CAPTURE=1`, and `HALO_IOS_TEST_METALFX=1` open the panel,
start capture, or request MetalFX respectively, without changing saved settings.

`HALO_IOS_TEST_RENDERER=metal` or `opengl` overrides the renderer for that
launch only. It does not change the user's saved preference.

The game and host are built from source. `tools/ios_angle.py` downloads the
pinned ANGLE runtime frameworks packaged by
[EdgeFirstAI/angle-package](https://github.com/EdgeFirstAI/angle-package),
version `v2.1.28252` / source `c053bf85793bbb83016b1196d04e5df3594b9bcc`.
The archive's SHA-256 is checked before extraction. Device and simulator
slices are embedded separately; device frameworks are signed with the app's
team. Public unsigned IPAs strip the vendor signatures from the embedded
copies. Framework binaries stay in ignored build directories. The package's
build scripts and pinned source revision are available upstream for rebuilding.
