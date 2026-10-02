# Browser invitations for native multiplayer

The source-built browser preview accepts the desktop game's `halo://join/<44 hexadecimal characters>` invitations. The equivalent shareable link uses HTTPS:

```
https://fqlx.github.io/halo-ce-universal/#join=<44 hexadecimal characters>
```

The launcher can paste native invites and copy browser links. The fragment is not included in HTTP requests or referrer headers. An invite grants access to its host's current session; it stops working after that host exits. A Discord channel URL or an expired Discord rich invitation is not itself a game token.

**Deployment status:** both public sites now serve the source-built runtime with the relay endpoint unset. Chrome has joined an actual release33 desktop host through a local relay and entered a two-machine Blood Gulch match. A public native relay still must be configured and tested before the public URL can join desktop games.

## Why the relay is required

Browsers cannot use the desktop build's raw UDP sockets. `port/web/site/gateway.js` carries the browser's virtual game sockets over a secure WebSocket. A session-specific worker in `port/relay` runs the existing native MQTT, encrypted P2P and KCP networking. It joins only the invited host and forwards game traffic; it does not run game simulation or need the ISO/maps.

The browser receives the worker's native identity and peer address mapping before starting its network stack. Once the maps and invited host are ready, the launcher automatically discovers and joins that host's game. This uses the production `HALO_QUICK_PLAY=join` path with the authenticated host's virtual address, without navigating System Link menus. A host being offline or unreachable is shown separately from a relay connection. **Main menu** cancels quick play.

Browser-only WebRTC rooms are also available in the source port. They have a different protocol and room code; putting a native invite into a browser room does not join the native host.

## Build the browser

Use Emscripten 6.0.10, Python 3 and Ninja:

```sh
source build/emsdk/emsdk_env.sh
python3 configure.py --release
ninja web
python3 tools/build_multiplayer_pages.py --relay wss://YOUR-RELAY-HOST/join
```

The output is `dist/browser-multiplayer`. All paths are relative, so it can be served under a GitHub Pages project or a preview subdirectory. An empty `--relay` produces an explicitly unavailable desktop-join state. Configure only an operator-controlled relay URL; links cannot override it. Relay access keys are entered in the launcher and sent in the first WebSocket message, never embedded in the public site or invitation.

The source-built engine includes current native network version 4. The existing `tools/build_pages.py` still packages the previous hash-pinned Apollo engine. Its WebAssembly has no multiplayer sockets, so adding a URL argument to that runtime cannot enable multiplayer.

## Deploy the relay

See [tools/browser-relay/README.md](tools/browser-relay/README.md) for build/run configuration and required limits. The relay needs a Linux host with outbound MQTT/STUN and UDP connectivity, and WSS termination. GitHub Pages serves static files and cannot run this process. Public deployments require an access capability and exact allowed browser origins. Private/reserved peer destinations are rejected by the production worker.

Runtime-only updates can ship with the relay unset. Before enabling a public relay, verify browser-to-native gameplay through that deployed endpoint. Update both the game's static branch and the game subdirectory in the personal-site mirror, preserving the personal site's root files and CNAME.

## Validation

```sh
node --test tools/tests/native-invite.test.cjs
npm ci --prefix tools/browser-relay
npm test --prefix tools/browser-relay
make -C port/relay test
```

The native fixture exercises the real encrypted P2P path with a local broker and echo host. The frontend tests exercise framing, session permissions, worker lifetime and backpressure. These are transport tests, not a Halo match: launch a compatible native host, open a fresh invitation in the browser, discover it under System Link, join the lobby, and play together before marking crossplay verified.

Verified locally on 2026-09-29: Chrome downloaded and cached all 24 maps, followed an HTTPS-style fragment invitation on loopback, authenticated the native host through the relay, joined System Link, and loaded Blood Gulch. The native host recorded two players on two machines, all machines loaded, and about 85 seconds of bidirectional gameplay packets; the browser rendered the match and reported live player updates. The tab subsequently disappeared and the host recorded connection loss; the cause of the tab disappearance is unknown. The native host was the official `build-33` Linux release (source `0ef2ed7`). This was a local, automated join test using `HALO_NETWORK_TEST=join`, not a public Internet/NAT test or a sustained gameplay/performance benchmark. Test arguments, game assets, native runtime packages and private invitations are excluded from source.
