# iOS/browser integration validation

This record applies to `apple/ios-web-multiplayer`, based on PR #12 at
`eaa82e6803f3d3e67c91d7f2fa2b15ee04e195f5` and the iOS fork at
`3f2c14101d3ae1c7f0c0a11993a43407fadbeb46`. Updated 2026-10-03.
Reports of gameplay on the original iOS fork are not validation of this merge.

## Observed locally

- Unsigned ARM64 iPhone/iPad app and IPA built with Xcode 27 and LLVM/LLD 23.1.2.
- ARM64 iOS simulator app compiled and linked, installed and launched on the
  iPhone 18 Pro / iOS 27 simulator. The native landscape XISO picker appeared.
  Startup stopped at the asset prompt; no game maps were available.
- Signed native ILP32 execution probe passed: layout, global/stack access,
  indirect calls, atomics and pointer zero-extension.
- Concurrent memory protection/tracking and zeroed reused mappings passed.
- Audio handoff passed 100 real SDL callbacks and 134,144 exact PCM samples.
- Display sizing and fifteen synthetic XISO regressions passed under sanitizers.
- Native virtual sockets passed packet framing, ring/counter wraparound,
  backpressure and ordered control-queue tests under ASan/UBSan.
- JavaScript suite: all 132 tests passed, including six new native adapter
  tests, including delayed acknowledgements, all 128 ping rows and migration/reconnect callbacks.
- A real macOS WebKit-to-Chromium WebRTC connection carried 4,096 exact datagram
  bytes and 262,144 exact ordered reliable-stream bytes from native virtual
  sockets and back through PR #12's transport.
  This tests the network bridge, not the game or an iPhone.

The shared browser build and relay CI passed at `30372ca8`; iOS and the
Android/Linux/Windows builds previously passed at `84b117aa`. See the fork
[Actions runs](https://github.com/AttilaTheFun/halo-ce-universal/actions)
for revision-specific results. Local host migration tests also pass with the
`HALO_IOS_BROWSER` feature guard and still reject the original failure cases.

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
- Use a browser game built from the pinned PR #12 revision with identical maps.
  Join from iOS, spawn, move/fire, and verify both clients see the same match.
- Repeat with iOS hosting. Exercise late join, disconnect/reconnect, host
  departure/migration, scores, player identity and checkpoint continuity.
- Test separate internet connections and TURN-only connectivity.
- Measure frame time, latency, memory and extended play on hardware.
- Exercise app background/resume and WebKit process termination; recovery is
  not yet a validated promise.

No real maps or physical device were supplied for this integration session.
The full game match and physical-device behaviors above remain unverified.
CI builds and transport tests must not be presented as proof of playable
native/browser crossplay. Keep the PR in draft until those checks pass.
