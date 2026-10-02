# Local browser runner

Run the published [bnunu Apollo browser beta](https://bnunu.itch.io/apollobeta)
locally with an opaque canvas fix, batched WebGL stream uploads, and render-state
caches.

For a hosted demo or to publish the same runtime on GitHub Pages, see
[GITHUB_PAGES.md](GITHUB_PAGES.md).

This is a runner for the publisher's pinned HTML build `19421784`, **not an
Emscripten source-build target for this checkout**. The setup script verifies
SHA-256 hashes before installing the runtime into ignored `build/web/` and
records provenance in `build/web/download-provenance.json`. No game maps,
disc images, downloaded runtime binaries, or Xbox SDK files are committed.

## Start

Requires Python 3.9+ and a recent Chrome with WebAssembly threads and WebGL 2.
From the repository root:

```sh
python3 tools/setup_browser.py
python3 tools/serve_browser.py
```

Open <http://127.0.0.1:8767/> and select the disc image of your own Xbox copy
of Halo: Combat Evolved. The publisher's importer stores the maps in this
browser once; nothing is uploaded. Later visits reuse the stored maps and
saves. Keep the same host and port to retain access to that storage.

Alternatively, serve maps directly from an existing supported disc image:

```sh
python3 tools/serve_browser.py --image '/path/to/Halo.xiso.iso'
```

Then open <http://127.0.0.1:8767/?data=/assets/> and click **Play**. This route
validates all 24 maps and supports NTSC build `01.10.12.2276` and PAL build
`01.01.14.2342`. The server reads only the requested map bytes from the image;
it does not extract an additional copy to disk. `run-browser.command` is a
shell shortcut for the server and accepts the same arguments.

The server binds to `127.0.0.1`, disables directory listings, serves the WASM
MIME type, and supplies the cross-origin isolation headers needed by threads.
Stop it with Ctrl-C. Re-run the setup script to restore the pinned runtime.

The publisher's Asyncify compatibility build is available with
`?build=asyncify` if the default JSPI runtime is unsupported.

## Rendering fixes

`port/web/opaque-canvas.js` requests `alpha: false` when creating the game
context. The Xbox renderer often disables alpha writes; a transparent browser
canvas otherwise hides valid rendered RGB content.

`port/web/stream-batch.js` records GL commands and merges append-only vertex
and index uploads before their draws. The supported rings are identified by
Halo's exact allocation sizes and usage. Uniform data and upload bytes are
snapshotted before the WASM heap changes; untouched gap bytes are preserved.
Overlaps retain their original order, and readbacks, transfers, and presentation
boundaries flush queued commands. GPU writes invalidate CPU shadow data.

The browser build uses three 16 MiB vertex buffers and three 2 MiB index
buffers. Batching avoids repeatedly modifying these large buffers between
draws, at the cost of approximately 54 MiB of CPU shadow memory. The shaders,
vertex values, published WASM, and engine JavaScript are unchanged.

`replay-cache.js` skips unchanged uniforms, constant vertex attributes, and
render state before the recorder copies their arguments. Uniform caches track
overlapping array ranges and invalidate on alternate setters and program
changes. `vertex-state-cache.js` separately tracks attribute enable bits per
VAO and sampler bindings per texture unit. Neither cache removes draws or
changes buffer uploads. Both run after the stream recorder is installed.

Use `?replay_cache=0&vertex_state_cache=0` to compare with batching alone;
either cache can also be disabled independently. Local helper script URLs carry
content hashes so re-running setup picks up changed code despite browser caches.
The local server also revalidates cached HTML and scripts on each visit.

This wrapper relies on Halo's append-only allocator contract; it is not a
general-purpose WebGL optimizer. Add `?batch_streams=0` to disable it for a
comparison. The FPS HUD counts engine presents rather than animation callbacks.
`?profile=1` enables GL call timing and adds measurement overhead.

## Local validation

On an Apple Silicon Mac in Chrome 153, with a 3022 × 1540 canvas and the
Silent Cartographer campaign:

| Scene | Original uploads | Batched uploads |
| --- | --- | --- |
| Early intro, frame range 10–80 | ~3.5 FPS | 21–29 FPS across two runs |
| Later intro | scene-dependent | roughly 48–100 FPS in the first run |
| First beach combat | not used for a matched comparison | roughly 25–30 FPS |

The baseline included attribute telemetry. Samples were collected once per
second; the early range contained 19 baseline readings and two or three batched
readings. The observed improvement is approximately 6–8× for that opening
scene, not a steady 120 FPS claim or a hardware-independent benchmark. Compare
matching views and warm shaders; background windows and scene changes affect
results. Textured rendering, firing, weapon switching, and pause/resume were
checked. This does not validate a full campaign or multiplayer.

A second rendering comparison on the same Mac used a 2450 × 1305 canvas,
with the beach scene held at the same pause menu throughout. Diagnostic buttons
changed the caches without reloading or moving the camera. Each row uses the
last ten one-second readings after allowing the setting to settle:

| State, in measurement order | Median FPS | Range |
| --- | ---: | ---: |
| Both caches enabled | 35.00 | 32.07–37.24 |
| Render-state cache only | 32.57 | 31.82–35.61 |
| Both caches disabled; stream batching retained | 27.78 | 24.75–29.39 |
| Both caches re-enabled | 34.29 | 30.43–36.38 |

This is a 23–26% improvement in that fixed rendering workload, not a claim
about every gameplay scene. An independent fresh gameplay run of the faster
render-state cache measured 35.36 FPS median over frames 2700–3300 versus
32.67 with the earlier batched build; moving combat scenes are less controlled.
The temporary live-toggle controls are not shipped. Larger command batches and
pooled uniform snapshots did not improve the live comparison and were excluded.

Recorder CPU overhead also fell by roughly 35% in a separate synthetic no-GPU
benchmark. It is reproducible with
`node tools/test_stream_batch.mjs --benchmark --baseline-ref=b9da047`; these
numbers are not game FPS. Reload after WebGL context loss; complete context
recovery is not validated by this runner.

Run the regression checks without game data:

```sh
node tools/test_stream_batch.mjs
node tools/test_replay_cache.js
node tools/test_vertex_state_cache.cjs
python3 -m unittest discover -s tools -p 'test_setup_browser.py'
python3 -m unittest discover -s tools -p 'test_serve_browser.py'
```

## Controls and launch options

- Click the canvas to capture the mouse; Esc releases it.
- WASD moves, mouse aims, left click fires, Space jumps.
- E uses, R reloads, F melees, Tab switches weapons, P pauses/resumes.
- Enter/Space accepts menus; Backspace goes back.

In the launcher's Options field, `(set terminal_render false)` hides beta
debug text. To start the Silent Cartographer directly, use:

```text
(set terminal_render false);map_name levels\b30\b30
```

Options persist. Remove the `map_name` command to return to the main menu.
The publisher build skips Bink movies and reports no network link; multiplayer
is not verified by this runner.
