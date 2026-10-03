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
