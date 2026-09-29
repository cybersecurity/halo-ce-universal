# Halo on macOS

Open **Halo Combat Evolved.app** from the build directory. Keep the original
game data in the configured data folder. The game uses the original Halo maps
and audio; this port does not distribute them.

Settings are saved at
`~/Library/Application Support/Halo Native AOT/save/config.toml`.
Quit the game, edit the file, then reopen it. Existing display settings control
fullscreen, window scale, interpolation and internal render width. F11 switches
fullscreen while playing.

| Action | Default keys |
|---|---|
| Move / look | WASD / mouse |
| Jump / fire | Space / left mouse |
| Zoom / grenade | Right mouse, Z or middle mouse / G |
| Use or reload | E or R (context sensitive) |
| Melee | F or Mouse4 |
| Change weapon / grenade type | Tab or wheel / X |
| Crouch / flashlight | Left Ctrl or C / Q |
| Pause | Escape |
| Menu navigation / confirm / back | Arrows / Return / Backspace |
| Release mouse / fullscreen | F12 / F11 |
| Quit normally | Command-Q |

Focus loss releases the mouse and clears held keys/buttons. Click or focus the
game again to resume. The pointer works in menus; mouse look applies in game.

To rebind, edit the `[input]` action strings, such as `jump = "Space"`,
`action = "E,R"`, `zoom = "Mouse2,Z"` or `grenade = "G"`. A comma separates
alternatives. `Mouse1` is left, `Mouse2` right, `Mouse3` middle; `Wheel` switches
weapons. `mouse_sensitivity` defaults to `1.0`; `invert_mouse` defaults to
`false`. An empty action string disables it. Tutorial control labels follow
keyboard bindings; connecting a gamepad retains controller icons.

Multiplayer source follows upstream protocol 4. Local system link uses
`network.online = false`. Router forwarding is controlled by
`network.allow_upnp`; set it to `false` to prevent UPnP requests. The upstream
128-player limit has not been tested with 128 clients on this Mac. Read
[compatibility notes](COMPATIBILITY.md) for the verified network
bridge tests and current crossplay, map/mod and invite-link limitations.

For rebuilding and diagnostics, see [Apple Silicon build instructions](https://github.com/pkyanam/halo-ce-apple-silicon#readme).
