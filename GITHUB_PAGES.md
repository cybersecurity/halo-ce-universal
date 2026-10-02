# Play Halo on GitHub Pages

- Game: <https://fqlx.github.io/halo-ce-universal/>
- Mirror: <https://abwburns.com/halo-ce-universal/>

Both sites serve the source-built browser port in `port/web`. The game runs
in a Web Worker and uses WebAssembly threads and WebGL 2. The GitHub URL
does not redirect to the personal domain.

**September 29, 2026 release update:** automatic match startup, one-player
System Link, host recovery, room switching and cached-launcher compatibility
are combined with the Chrome/macOS presentation and repeated-rejoin fixes.
Use each site's `deployment.json` for its current source commit, runtime and
packaged file hashes. Cached launchers remain compatible with the prompt-free
page while the service worker updates. Existing visitors can reload and choose
**Update**; imported maps and saves are retained.

Chrome-family browsers on macOS use owned RGBA frame transfers instead of the
ImageBitmap serialization path that crashed in Chrome 153. Frame queues are
bounded, empty bitmaps are rejected, and same-address reconnects retire the
old RTC/native connection before accepting its replacement. Departed players
release input ownership while retaining their scores. A distributed browser
Free-for-All Slayer host can keep waiting when an opponent reloads or leaves.

Historical tests on the earlier runtime `0780c5587f467c15` started a solo
match and joined it with a second player, then hit renderer crashes
("Aw, Snap", error 11). Those failures describe the old runtime. The later
local crash/rejoin build passed an initial late join and five consecutive
same-address reload/rejoin cycles; the same host stayed playable for over
six minutes. That run predates combining these fixes with the host-recovery
and room changes. The combined release passed `ninja web`, all 104 browser
tests, cache and stream-recorder checks, and native lifecycle/restart tests
under AddressSanitizer and UndefinedBehaviorSanitizer. These checks do not
verify physical-iPhone gameplay or connections across different Internet/NAT
networks.

This replaces the pinned Apollo runtime, which froze during game startup
in desktop WebKit 26.4. The worker-based port reached the Halo main menu
with the same UI map in the September 29, 2026 test. Touch controls are
available; startup and gameplay on a physical iPhone remain unverified.
See [port/web/README.md](port/web/README.md) for requirements and controls.

Browser rooms need `ui.map` and `beavercreek.map` before starting. The full
main menu and desktop invites need the complete map set. Each player imports
their own Xbox disc image through the launcher; the site does not provide
maps or an ISO. Returning visitors reuse maps already stored in that browser.
The September 29 local Chrome check used a previous version that downloaded
only `ui.map` and `bloodgulch.map`; it does not verify the current import flow.
Existing Apollo maps and saves are reused in place under `halo/data` and
`halo/save`. Each browser and domain has a separate cache. Browser storage
can be cleared or evicted.

## Default browser room

The launcher automatically joins the public browser room **FQLX01** on a
first visit. Share <https://fqlx.github.io/halo-ce-universal/?room=FQLX01>
to enter that room directly. The mirror uses the same room, so players on
both domains can meet. An explicit room link or a previously chosen room
takes precedence; choosing **Leave** keeps the browser out across reloads
until the player opens a room link or chooses **Join default room**.

After the UI and Beaver Creek maps are ready, the launcher starts multiplayer automatically.
The first ready participant hosts **Beaver Creek Slayer**; later participants
join that host, including while the match is running. Only players preparing
to launch participate in host selection, so an idle tab or a disc import does
not become the host. The public room is not a persistent game server.

The source migration path preserves the loaded match when its host leaves.
It pauses gameplay, elects a survivor with a verified checkpoint, and reconnects
players into their existing slots under the replacement host. Scores, positions,
inventory and the timer are retained. Recovery failure leaves the match paused;
it never starts a new match automatically. All participants need the matching
new launcher and runtime (native network version 5); apply the offered update
on every participating browser.

A brief connection loss has a 10-second grace period, and silent channels are
detected after 25 seconds, followed by election and reconnecting. Authority-only
state may resume from the latest 15-tick checkpoint. Returning hosts yield to
the newer room epoch. Public signaling does not provide distributed consensus;
partitions can still create divergent continuations of a match.

Hold **Tab** (or **F1**) to show the scoreboard. **(HOST)** identifies the
current host, and **Ping** shows each player's round-trip latency to that host
in milliseconds. Missing or stale samples show `--`; host migration clears
the previous measurements. Blank or whitespace-only player names receive a
unique random name, including players joining a match in progress.

The page shows connection and loading progress. A browser may require a tap
to enable sound and pointer capture, but no System Link menu navigation is
needed. Manual browser System Link also allows a single player to start a
non-team game with distributed networking; the game stays open for later
players. Team readiness and the two-machine requirement for lockstep remain.
**Main menu** opts out of quick play; `?menu=1` opens the normal
launcher and asks for a disc image if the full map set is missing. Leaving a
quick-play match for the menu also checks for those maps, so other scenarios
cannot be selected before their data is available. A failed attempt shows an error instead of repeatedly
restarting the game. Private room links use the same quick-play flow.

Use the in-game **Room** button to open the room controls, then choose
**New room** or enter another code. Switching rooms restarts Halo into the
selected room using cached maps. **Share link** includes the currently
joined room; a `?room=FQLX01` link always selects FQLX01. **Leave** closes
the current game and returns to the launcher without automatically rejoining.

Browser rooms use the existing public signaling and STUN services. Some
networks still require a TURN relay in Settings; no TURN account or native
relay is configured by the default room. The room code is public, so use
**New room** for a separate group.

Native desktop invitations require a separately hosted WSS relay. The
public sites currently leave that endpoint unset. Chrome has joined a
native match through a local test relay, but the public link cannot yet
join native hosts. See [BROWSER_NATIVE_INVITES.md](BROWSER_NATIVE_INVITES.md)
for the protocol, deployment requirements and validation limits.

## Build and package

Use Emscripten 6.0.10, Python 3 and Ninja:

```sh
source build/emsdk/emsdk_env.sh
python3 configure.py --release
ninja web
python3 tools/build_multiplayer_pages.py
```

The output is `dist/browser-multiplayer`. It contains the runtime, launcher
and service worker, with no ISO or maps. `deployment.json` records the
source commit, build state and each packaged file's SHA-256. Commit source
before packaging. Add `--relay wss://YOUR-RELAY-HOST/join` only when that
relay is ready; an unset relay leaves desktop joining unavailable.
Use `--default-room CODE` to choose a different public room or
`--default-room ''` to disable automatic room entry.

The launcher copies maps from each player's own disc image into browser
storage. The public package and multiplayer relay do not serve game assets.
Game assets retain their own copyrights and are not covered by the source-code license.

## Publish

Publish the generated package to `fqlx/halo-ce-universal`, branch
`fqlx/pages-static`, and to the `halo-ce-universal/` subdirectory of
`fqlx/abwburns-site`, branch `fqlx/site`. Keep both branch histories.
Preserve the personal site's root files and `CNAME`. The account repository
`fqlx/fqlx.github.io` has no custom domain, keeping the game URL on GitHub.

HTTPS, service workers, WebGL 2, browser file storage and shared memory
must be available. The service worker supplies cross-origin isolation
headers that GitHub Pages cannot configure directly, and caches each
runtime version together. Reload after deployment and apply an offered
update. Do not clear website data just to update the runtime, since that
also removes imported maps and saves.

Use the direct play URL when sharing. An iframe's parent also needs
cross-origin isolation and appropriate permissions; embedding the link
in an arbitrary page cannot enable threads by itself.

For a local static-host check without special server headers:

```sh
python3 -m http.server 8778 --bind 127.0.0.1 --directory dist/browser-multiplayer
```

Open <http://127.0.0.1:8778/>. The service worker establishes isolation and
reloads once, then the launcher checks browser features and map storage.
Run `node tools/test_source_cache.mjs` and
`node --test tools/tests/*.test.cjs` for cache and invitation checks.

## Previous Apollo runner

`tools/build_pages.py` and [BROWSER_LOCAL.md](BROWSER_LOCAL.md) remain
available for reproducing the older hash-pinned Apollo runtime and its
rendering wrappers. That packager outputs `dist/github-pages`; it is
separate from the source-built runtime now hosted on the public sites.
It packages no game data and has no automatic map download option.
Apollo performance measurements do not establish performance of the newer
source-built engine, and its original runtime has no multiplayer sockets.
