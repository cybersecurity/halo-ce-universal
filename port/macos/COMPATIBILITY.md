# Native macOS compatibility

The selected game, network and renderer updates are taken from
[cybersecurity/halo-ce-universal](https://github.com/cybersecurity/halo-ce-universal)
main commit `0ef2ed7dc7a96299ea7d8d02a1bc3f9fb1c231af` (checked 2026-09-29).
The previous source snapshot was `dddaffd4`. The three-way integration record
and preserved local files are in `build/upstream-merge-0ef2ed7d/`.

## Multiplayer

This source uses upstream network protocol **4**, including distributed
simulation, joining a game in progress, matching host player datum slots,
and PAL map normalization. It retains the original guest packet encoding,
32-bit game records, 30 Hz simulation, map checks and network version checks.
It does not substitute host C structs for game records.

Upstream defines a maximum of **128 players on 128 machines**. That is a
source limit, not a test result for 128 active clients. Native tests have
passed TCP and UDP localhost payloads through the actual guest import ABI,
including Linux/Xbox IPv4 addresses converted to Darwin, accepted peer and
received source addresses, select read/write lists, and checked output bounds.
These tests do not establish a successful game join or cross-platform session.
A matching protocol-4 counterpart and game-level host/join test are still
required before claiming verified crossplay. Older protocol-1 builds are
incompatible; the normal upstream version mismatch message is preserved.

For local system link testing, use `network.online = false`. Upstream's
`debug.network_test = "host:bloodgulch"` and `"join"` automate real sessions.
The native bridge includes upstream miniupnpc router discovery/UDP mapping.
`network.allow_upnp = false` prevents requests to a router; enable it only
when desired. No router forwarding or internet/NAT traversal has been verified
on this machine.

Five musl process-spawn imports remain explicitly unbound: `posix_spawn` and
its four file-action helpers. The current Mac platform excludes the Linux
URL installer/updater command paths that would use them. They are not dummy
success implementations. Native URL scheme registration and command-line
invite discovery also remain unavailable through this bridge; do not promise
invite-link installation or Linux updater compatibility on macOS.

## Maps, mods and visuals

The port keeps upstream map/tag loading and game data layouts. It includes
upstream PAL tag normalization and its gameplay, HUD, grenade and renderer
fixes. This is compatibility with the upstream game's data model, not support
for arbitrary Halo PC/Custom Edition DLL plugins, SAPP, Windows executables,
or every modded map. Compatible map/tag content still has to pass the same
upstream format/version checks and be tested with the matching session data.

Existing settings control display refresh interpolation (enabled by default),
fullscreen, window scale and the Mac internal `display.render_width` option.
Interpolation draws between 30 Hz game ticks; it does not change the network
tick rate. Renderer updates preserve the Mac OpenGL presentation path.
There is no newly claimed shader-mod or PC plugin API.

Measured native performance has reached 59.5–60 FPS outside the A10 cryo
chamber, but the chamber itself can render near 30 FPS. This is a scene
performance limit, not proof of steady 60 FPS everywhere. With interpolation
and vsync enabled, the source bypasses the Xbox throttle, pending-flip wait,
and 30 Hz render selection. There is one SDL swap per presented frame.
CPU work that misses a display deadline can move presentation to the next
refresh; a 15–16 ms swap alone does not prove a second pacing wait. No
speculative timer or simulation-rate change was made to hide this limit.


## Mac controls

The keyboard and mouse drive player one, alongside an optional SDL gamepad.
Default controls: WASD movement, mouse look, Space jump, left mouse fire,
right mouse/Z/middle mouse zoom, G grenade, E or R context action/reload,
F or Mouse4 melee, Tab/wheel weapon switch, X grenade type, Q flashlight,
and Left Ctrl/C crouch. Arrow keys and Return navigate/confirm menus;
Backspace goes back; Escape pauses; F11 toggles fullscreen; F12 releases or
recaptures the mouse; Command-Q uses the game's normal exit path.

`config.toml` has `[input]` string entries `forward`, `backward`, `left`,
`right`, `jump`, `fire`, `zoom`, `grenade`, `action`, `melee`, `change_weapon`,
`change_grenade`, `flashlight`, and `crouch`. For example:

```toml
[input]
action = "E,R"
zoom = "Mouse2,Z"
grenade = "G"
mouse_sensitivity = 1.0
invert_mouse = false
```

Bindings accept comma-separated physical letters, digits, Space, Tab,
Enter, Escape, Backspace, LeftCtrl/Ctrl, LeftShift/Shift, LeftAlt/Alt,
Up/Down/Left/Right, Mouse1 through Mouse5, and Wheel for weapon switching.
An empty string disables an action. Invalid values log a warning and use
the default. Settings are read at startup; restart after editing. Gamepad
button preferences remain game preferences; keyboard actions retain their
original controller action semantics (interaction and reload share X).

On focus loss, key presses, mouse buttons, accumulated motion and wheel input
are cleared and cursor capture is released. Capture resumes on focus regain
when desired by the player. HUD control icons display the configured first
key/mouse binding, Mouse for look and WASD for movement; a connected physical
gamepad retains the original controller icons. Original map assets are not edited.

The x87 runtime models registers as host doubles. Exact zero and ordinary
parser examples are tested, but the original binary80 extra precision is not
fully preserved. This existing translation limit is not a networking format change.
