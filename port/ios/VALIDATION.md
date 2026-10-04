# iOS/browser integration validation

This record applies to `apple/ios-web-multiplayer`, based on PR #12 at
`eaa82e6803f3d3e67c91d7f2fa2b15ee04e195f5` and the iOS fork at
`3f2c14101d3ae1c7f0c0a11993a43407fadbeb46`. Updated 2026-10-03.
Reports of gameplay on the original iOS fork are not validation of this merge.

## Observed locally

- Unsigned ARM64 iPhone/iPad app and IPA built with Xcode 27 and LLVM/LLD 23.1.2.
- ARM64 iOS simulator app compiled and linked, installed and launched on the
  iPhone 18 Pro / iOS 27 simulator. With the user's NTSC-US v5 maps
  (`01.10.12.2276`), the native main menu and Beaver Creek gameplay rendered;
  the audio callback reported active PCM output. This is simulator evidence,
  not physical-device performance or a listening test.
- The full-disc ISO uses partition offset `0x18300000`. Native extraction and
  the inspection helper produced byte-identical results for all 24 maps
  (1,860,638,720 bytes); SHA-256 hashes were compared locally. The helper now
  recognizes that offset, which the native and browser importers already used.
- Real game crossplay with Chromium's WASM build from CI revision `221ba03e`:
  the browser joined the native host's running match at tick 2991, then the
  native client joined a fresh browser-hosted match at tick 592. Both rendered
  Beaver Creek and the host logs confirmed the remote player finished loading.
  Both processes ran on the same Mac, using public signaling and WebRTC.
- Host departure worked in both directions: after terminating iOS, Chromium
  adopted the live match at tick 4542 / epoch 1; after closing the browser host,
  iOS adopted the live match at tick 1279 / epoch 1. The engine logged preserved
  match state. Score, movement/fire replication and extended continuity have
  not yet been systematically verified.
- A cold restart of the original iOS host could not join its former match
  after the browser took over: the browser server rejected the new connection
  because its selected machine slot was occupied. This is a known failing
  scenario, distinct from live-client host migration and fresh-room joining.
- Signed native ILP32 execution probe passed: layout, global/stack access,
  indirect calls, atomics and pointer zero-extension.
- Concurrent memory protection/tracking and zeroed reused mappings passed.
- Audio handoff passed 100 real SDL callbacks and 134,144 exact PCM samples.
- Display sizing and sixteen synthetic XISO regressions passed under sanitizers.
- Native virtual sockets passed packet framing, ring/counter wraparound,
  backpressure and ordered control-queue tests under ASan/UBSan.
- JavaScript suite: all 132 tests passed, including six new native adapter
  tests, including delayed acknowledgements, all 128 ping rows and migration/reconnect callbacks.
- A real macOS WebKit-to-Chromium WebRTC connection carried 4,096 exact datagram
  bytes and 262,144 exact ordered reliable-stream bytes from native virtual
  sockets and back through PR #12's transport.
  This tests the network bridge, not the game or an iPhone.

All three CI workflows passed at `221ba03e`: browser/relay/tests, iOS unsigned
IPA/simulator/probes, and Android/Linux/Windows builds. See the fork
[Actions runs](https://github.com/AttilaTheFun/halo-ce-universal/actions)
for revision-specific results. The asset-based simulator tests above used the
same engine with simulator-only launch settings added to bypass the chooser;
those settings do not affect device builds. Local host migration tests also
pass with the `HALO_IOS_BROWSER` feature guard and reject original failure cases.

## Menu and controller follow-up

- Normal launch now opens Halo's main menu. The signed update was installed on
  the designated iPhone 13 Pro (iOS 26.7.1); its screenshot confirms the menu,
  and its log reports 194 resolved imports and active PCM output.
- System Link's menu handler now opens the room chooser after startup. The
  native form uses the keyboard layout guide and a scrollable field area;
  a landscape simulator screenshot with the keyboard visible confirms the
  code field and both action buttons remain on screen.
- The real quick-play state machine passes deferred-start and repeat-start
  tests; stale packet/control/ping state is cleared between room selections.
  A simulator room launch still reaches Beaver Creek gameplay.
- SDL hardware-controller routing retains the connected primary gamepad,
  balances its owned open/close reference, logs changes and automatically
  hides/restores touch controls. The simulator's gamepad triggers hiding;
  the user subsequently confirmed an Xbox Series X controller works on the
  phone. Backbone/PlayStation hardware behavior remains unverified. The touch
  visibility toggle now hides along with the controls while connected.
- The iPhone 15 Pro is excluded from further device testing at the user's
  request. Physical-device tests now target only the iPhone 13 Pro.

## Reproduce

```sh
python3 tools/ios_test.py
node --test tools/tests/*.test.cjs
python3 tools/ios_build.py --unsigned --ipa dist/Halo-CE-iOS-unsigned.ipa
python3 tools/ios_build.py --simulator
```

Optional real-network transport test on an Apple Silicon Mac with a GUI session:

```sh
npm install --prefix build/network-test playwright
build/network-test/node_modules/.bin/playwright install chromium
node tools/test_ios_webrtc.cjs
```

This creates a random isolated room on the public signaling services, starts
Chromium and a native WebKit probe, and checks datagrams and reliable streams.
It needs internet access but no game assets or signing account. It is not run
by default CI, because external signaling and GUI availability are variable.

## Required before marking ready

- Install a personally signed build on an iPhone/iPad and import the user's
  original maps. Validate menu, campaign, controls, rendering and audio.
- Repeat the successful simulator join/host/migration tests on hardware with
  matching browser maps. Verify movement/fire replication, scores, player
  identity and checkpoint continuity over extended play.
- Resolve cold-restart admission after host migration; verify reconnect paths.
- Test separate internet connections and TURN-only connectivity.
- Measure frame time, latency, memory and extended play on hardware.
- Exercise app background/resume and WebKit process termination; recovery is
  not yet a validated promise.

The user supplied a compatible original Xbox disc image. Real native/browser
matches now have simulator evidence; physical-device behavior and the remaining
cases above are still unverified. Keep the PR in draft until those checks pass.

## Graphics diagnostics (2026-10-03)

- Fixed GLES water draws losing their destination framebuffer after ripple
  mipmap blits. A/B simulator runs of `levels\b30\b30` show a flat dark water
  surface with the old ordering and ripples after restoring targets/raster
  state following texture preparation. Other existing simulator scene artifacts
  remain; this is not a claim of complete rendering correctness.
- iPhone 13 Pro executed MetalFX spatial scaling from 1558×720 to 2532×1170.
  The menu rendered with the Metal-backed presentation path. This establishes
  execution, not a sustained-performance improvement.
- Simulator debug panel and system share sheet rendered in landscape.
  After dismissal the controls regain first-responder status; injecting the
  UIKit shake event reopens the debug panel. Physical shake input remains a
  user check.
  An exported report parsed successfully with 57 source records and 44
  first-use draw waits; every event source reference resolved. The simulator
  correctly disables MetalFX. Actual AirDrop delivery requires user selection
  on the phone and has not been tested by automation.
- Runtime ABI, memory, audio, display sizing, 16 XISO tests, and room bridge
  regression checks pass. Full signed-device and simulator host builds pass.
- macOS menu routing and a separate native macOS renderer remain unvalidated.

## Persisted renderer selection (2026-10-03)

- Default startup selects ANGLE's Metal backend. Simulator logs identify its
  Metal renderer; iPhone 13 Pro logs identify the Apple A15 Metal GPU. The
  alternative path uses Apple's EAGL/OpenGL ES driver, with separate symbol
  dispatch throughout guest rendering, shader diagnostics, and presentation.
- MetalFX runs on the iPhone 13 Pro using an ANGLE IOSurface → MetalFX handoff
  at 1558×720 → 2532×1170. It is unavailable in OpenGL mode and in the simulator.
- Normalize decoded texture uploads to RGBA on iOS. This fixes the red/blue
  reversal seen with texture swizzling in the ANGLE simulator. Silent
  Cartographer now shows blue sky, textured geometry and water ripples.
- Simulator UI control actions save OpenGL and remove the MetalFX row; the
  following launch logs OpenGL. Metal selection restores the row and the next launch logs Metal; it is saved
  independently of the upscaling preference. Changes apply on relaunch.
- Signed device, simulator, and unsigned IPA builds pass. The public IPA
  contains unsigned embedded framework copies and bundled license notices.
  Native runtime/layout, memory, audio, display, XISO and room checks pass.
- A Metal shader-report export identifies the ANGLE backend and validates
  all source references (13 shader sources, 7 recorded events in the menu run).
- This adds a Metal backend via ANGLE, not a handwritten Metal renderer.
  Extended campaign/crossplay performance and all-device coverage remain open.

## Private-image storage and native Mac target (2026-10-03)

- Removed the touch visibility toggle; iPhone controls remain automatic and
  the native AppKit target creates no touch overlay.
- Imported the user's full 7,825,162,240-byte disc through the shared private
  storage transaction. SHA-256 readback matched; all original maps validated.
- Six synthetic storage tests pass: retained image and restart without source,
  cancellation preserving current data, corrupt import preserving current data,
  truncated stored image rejection, pointer traversal rejection, and cleanup
  restricted to owned unpublished generations. Existing 16 XISO parser tests
  and ABI/memory/audio/display/room tests also pass.
- iPhone 13 Pro: native first-launch import screen observed; a verified private
  image generation was then transferred to its container for migration testing.
  Relaunch reached Metal main menu with 198 resolved imports, touch controls
  visible, and no hide/show toggle. The full Files-picker import transaction
  was exercised in the shared native importer, not driven end-to-end by taps
  on this phone. iPhone 15 Pro was not used.
- Mac: native main-menu and Silent Cartographer framebuffers captured, with
  audio. Developer ID/hardened-runtime app launched successfully inside its
  sandbox using its private Application Support generation. Initial MetalFX
  testing found an inactive-window lookup; using the SDL Cocoa window fixed
  that, with scaler execution confirmed. Performance and interactive controller,
  menu, and crossplay coverage remain outstanding.
- Simulator build passes, but this session's simulator launch service stalled;
  the device checks above replace that attempted UI smoke test, not a claimed
  simulator pass.
- Unsigned IPA and signed DMG contain no user disc image or maps. Replacement
  HUD/title/font payloads are disabled in Apple guest builds, and the app icon
  is original geometric art. Signing does not establish legal clearance.
- The ring-artwork Mac release built from `55bbbf7e` was accepted by Apple
  on 2026-10-03. App submission: `2ee63744-83dd-4188-8f02-3c3b74697397`;
  DMG submission: `890c6e2f-6bdb-4678-883f-e5b452746859`. Both tickets were
  stapled and validated. Gatekeeper accepted the final DMG and the app mounted
  from it as `Notarized Developer ID`. The final package was also checked for
  accidental game/private data. Actual AirDrop delivery remains unverified.

## Settings and menu audio update (0.1.4 / 8)

- macOS Settings is a reusable separate window under the application menu
  (⌘,); iOS retains its shake-presented modal, now titled Settings.
- Reproduced music starvation in a real eight-second native menu tracking
  session: servicing DirectSound completions alone drained to zero PCM.
  Servicing the guarded sound streaming update on the guest main thread kept
  PCM active throughout two subsequent eight-second menu sessions and after
  closing. The SDL worker never invokes the game's completion callbacks.
- Fixed black generated icon assets by replacing offscreen AppKit drawing
  with CoreGraphics resizing. Visually inspected the regenerated ring icon;
  both Mac ICNS and iOS catalog now use the correct bitmap.
- Native ABI, memory, audio callback/deadlock, display sizing, 16 XISO cases
  and room bridge tests pass. Both physical-device and Mac release builds pass.
- At the user's updated request, installed 0.1.4 (8) on iPhone 15 Pro
  (`iPhone16,1`, A17 Pro, iOS 27.0.1) and copied the verified private disc/map
  generation into its app container. Normal launch reached the main menu,
  with 199 resolved imports, Metal rendering, active PCM output and touch
  controls. Existing saves were preserved. No controller was connected during
  this smoke test; the earlier iPhone 13 Pro controller evidence is unchanged.
- The Mac app from `37ccd843` was accepted by Apple (submission
  `a0576c71-190f-47fd-9982-698422a89e1a`). Its ticket was stapled and validated;
  Gatekeeper accepted it as Notarized Developer ID. Packaged the stapled app
  as `HaloCE-0.1.4-AppleSilicon.zip` and successfully sent it to the user's
  MacBook through Taildrop. No DMG is required for this local test release.

## First-mission feedback / 0.1.5 (9)

- User completed the first mission on Mac and reported compilation hitches,
  two transparent surfaces (floor panel and escape-pod front), escape-pod
  jitter in the ending cinematic, and nonfunctional Dock Quit.
- The received report identified build 7 / `55bbbf7e`, ANGLE Metal on M4 Max,
  158 sources, zero timing events and zero dropped events. Replaying all 158
  shader sources with the bundled ANGLE driver on M6 compiled successfully.
  This does not rule out link, texture, material, draw-state or geometry bugs
  on the reported M4 Max. The two visual bugs are still unresolved; no
  speculative rendering or animation patch has been applied.
- Fixed AppKit creation order: initializing SDL video first installs
  SDL3Application and its standard app menu. Before this, pre-creating
  NSApplication skipped both menu setup and the terminate-to-quit bridge.
  Reuses SDL’s Settings placeholder to avoid AppKit stealing Cmd+, from a
  duplicate menu item. Real application-menu Quit and the Dock termination selector both produce
  a guest quit event and exit 0. Settings is tested through the actual menu.
- Added a bounded, GPU/OS/driver-scoped persistent ANGLE blob cache. A fresh
  launch loaded a compiled blob from disk. Callback tests cover disk reload,
  small/negative output buffer sizes, concurrent writes and 64 MiB eviction.
  First-use Metal pipeline specialization can still hitch; no frame-time
  performance claim is made from cache hits alone.
- Schema 2 shader reports record compile/link stalls even with optional GPU
  recording off, and include program/source associations. A real menu run
  exported a valid report with 11 programs / 13 sources, all references
  resolved, and a timing event while first-use GPU recording was false.
- The signed sandboxed 0.1.5 app launched with its private image store,
  populated its shader cache, opened Settings through the application menu,
  and exited 0 through Quit. Native menu audio remained active through an
  8.24-second tracking session. Mac and iOS device builds and runtime/cache
  probes passed; this update was not installed on an iPhone.
- Apple accepted the app from `1235f980` (submission
  `47e2d143-3182-4e31-aa4b-43bdd1e69ff3`); its ticket was stapled and Gatekeeper
  accepted it as Notarized Developer ID. Sent `HaloCE-0.1.5-AppleSilicon.zip`
  to the user's MacBook via Taildrop. The visual mission bugs above remain
  open pending a reproducible scene/checkpoint or clip.
