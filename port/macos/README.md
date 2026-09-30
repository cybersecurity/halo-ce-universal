# macOS

`ninja macos` builds the game for Macs with Apple silicon (M1 and later),
as native arm64 code. `ninja macos_app` makes an application from it:
`build/macos/Halo.app`. `ninja macos_x86_64` builds the game for Intel Macs
(it also runs on Apple silicon through Rosetta 2).

The game draws with OpenGL ES 3 through ANGLE on Metal. It plays sound
through SDL3. It accepts a keyboard and mouse, and game controllers (Xbox,
PlayStation, Switch Pro and other controllers that macOS knows). The game
needs macOS 14.4 or later.

## Requirements

- Xcode or the Xcode Command Line Tools (clang, lipo, codesign).
- Python 3.9 or later, and these packages: `pip3 install --user ninja cmake ziglang`.
  The `ziglang` package supplies `ld.lld` and `llvm-ar`, which link the
  game's image. An `ld.lld` and `llvm-ar` on the `PATH` operate too.
- ANGLE, universal (x86_64 and arm64): `libEGL.dylib` and
  `libGLESv2.dylib`. `configure.py` takes them from Steam's or Google
  Chrome's Chromium Embedded Framework if one is installed. The option
  `--macos-angle <folder>` selects a different folder.
- A network connection for the first build. `configure.py` downloads musl
  1.2.5 (its SHA-256 is checked), SDL 3.4.16 (the release commit is
  checked) and the Khronos OpenGL ES headers to `build/macos/third_party`.

## Build and start the game

1. Go to the root folder of the repository.
2. Enter `python3 configure.py`.
3. Enter `ninja macos_app`.
4. Open `build/macos/Halo.app`, or enter `open build/macos/Halo.app`.

### Sign and notarize the application

To give `Halo.app` to other people, sign it with a Developer ID, and
notarize it so that macOS opens it without a warning:

1. One time: `xcrun notarytool store-credentials halo-notary --apple-id <Apple ID> --team-id <team>`.
   It asks for an app-specific password (made at appleid.apple.com).
2. `NOTARY_PROFILE=halo-notary port/macos/sign_app.sh`

The script signs with the hardened runtime and
`port/macos/Halo.entitlements`, notarizes, staples the ticket and writes
`build/macos/Halo-macos-arm64.zip`. Without `NOTARY_PROFILE` it only signs.

`ninja macos` builds the same game without the application bundle, in
`build/macos/Halo`. Start it with `build/macos/Halo/halo`.

## Game data

The game needs the `maps/` folder of the Xbox game (not the Windows game).
Make a disc image (`.iso` or `.xiso`) of your own Xbox disc of Halo: Combat
Evolved. A PC DVD drive cannot read an Xbox game disc: use a modified Xbox,
or an Xbox 360 DVD drive with the Kreon firmware, and a disc image tool
such as extract-xiso.

1. Start the game.
2. At the first start, the game asks for the disc image. Select it.
3. The game extracts `maps/` (approximately 2 GB). Then the game starts.

| Build | Location of `maps/`, `config.toml`, `debug.txt` and `host.txt` |
| --- | --- |
| `Halo.app` | `~/Library/Application Support/Halo` |
| `build/macos/Halo/halo` | `build/macos/Halo`, next to the executable |

The saved games are in `~/Library/Application Support/Halo` for both.
`paths.data` and `paths.saves` in `config.toml` select other folders.

## Controls and settings

The controls and most of the settings are those of the Linux build. Refer
to [port/linux/README.md](../linux/README.md). The keys that the macOS
build adds:

| Key | Mac keyboard | Function |
| --- | --- | --- |
| F4 | ⌘J | The characters' and vehicles' shapes in the rays: their drawn models or their collision models. |
| F5 | ⌘L | The ray probe: draw the rays the lighting sends from the surface at the crosshair; again to freeze them in place (walk round them); again for off. |
| F6 | ⌘B | Step through what the ray tracing shows: the lighting, the ray view, split, the occlusion (refer to "Ray-traced lighting"). |
| F7 | ⌘P | Show or hide the frames-a-second counter. |
| F8 | ⌘R | Change the resolution: native, 2160p, 1440p, 1080p, 720p, then the Xbox's 640x480. |
| F9 | ⌘T | Switch the ray-traced lighting on or off. |
| F10 | ⌘, | Open or close the settings overlay (refer to "The settings overlay"). |
| F11 | ⌘F | Switch between fullscreen and a window. |
| F12 | ⌘G | Release or capture the mouse. |
| | ⌘X | The next camera: the first person, a flying camera, one following the player, and back (the debug cameras, under "Ray-traced lighting"). |
| | ⌘Z | In the flying camera, take its controls or let go of them: WASD moves, the mouse turns, Space rises, left ctrl or C sinks, shift (or Q) goes twelve times as fast. |

⌘Q quits on a second press within two seconds (Q is the flashlight, and ⌘
is held for the keys above); the window's close button quits at once.

On a Mac keyboard, F11 shows the desktop and F8 to F12 are media keys
unless fn is held, so the Command shortcuts do the same. Each key shows
what it did in the game's console at the top of the screen (for example
`ray tracing: on`), and writes it to `debug.txt`.

`HALO_FULLSCREEN=false` (or `display.fullscreen = false`) starts the game
in a window:

```bash
open --env HALO_FULLSCREEN=false --env HALO_FPS=1 build/macos/Halo.app
```

These settings are new, or have a different default on macOS:

| Setting | Default on macOS | Function |
| --- | --- | --- |
| `display.resolution` | `"1080p"` | The picture's pixels. `"native"`: the display's in fullscreen, the window's in a window. `"720p"`, `"1080p"`, `"1440p"`, `"2160p"`: that many lines, in the shape of the display or window. `"<width>x<height>"`: that picture. `"xbox"`: 640x480. |
| `display.show_fps` | `false` | Start with the frames-a-second counter shown (F7 / ⌘P). |

Every setting can also be given on the command line, over `config.toml` and
the `HALO_*` variables: `--display.vsync=false`, or by the last part of its
name alone, `--vsync=false` (a dash for an underscore: `--hidden-window`);
a boolean alone is true (`--mute`), `--no-` before it false
(`--no-vsync`); and short names `--gi path`, `--rt off`, `--map c10`,
`--fps`, `--windowed`. `--help` lists them all:

```
build/macos-release/Halo.app/Contents/MacOS/halo --gi path --no-vsync --fps --map c10
```
| `display.render_scale` | `1.0` | Multiplies the resolution: below 1.0 is faster, above 1.0 supersamples (up to 4.0). |
| `display.ray_tracing` | `"on"` | The ray-traced lighting (refer to "Ray-traced lighting"). `"screen"`: without Metal's rays. `"off"`: off. `"occlusion"` and `"depth"` show what the lighting uses. |
| `display.ray_tracing_occlusion` | `0.8` | How much the traced occlusion darkens corners and creases (0.0 to 1.0). |
| `display.ray_tracing_reflections` | `0.25` | How strongly the surfaces reflect the traced scene (0.0 to 1.0). |
| `display.ray_tracing_bounce` | `0.25` | How much light one traced bounce carries between surfaces (0.0 to 1.0). |
| `display.ray_tracing_shadows` | `1.0` | How dark the sun's traced shadows are (0.0 to 1.0). |
| `display.ray_tracing_objects` | `true` | The characters and vehicles in Metal's rays too: their contact shadows, and your own body's shadow. |
| `display.ray_tracing_gi` | `"traced"` | The level lit by the rays in place of its lightmaps (refer to "The traced light, in place of the lightmaps"). `"black"`: without the lightmaps at all. `"off"`: the lightmaps. |
| `display.ray_tracing_gi_split` | `false` | The game's light on the left half of the screen, the traced on the right. |
| `display.ray_tracing_level` | `"render"` | The level in the rays as it is drawn; `"collision"`: its collision surfaces. |
| `display.ray_tracing_lights` | `"traced"` | Every light traced in its colour; `"game"`: the game's lights, their shadows traced. |
| `network.allow_upnp` | `false` | The local network does not need a forwarded port. |
| `network.join_from_clipboard` | `false` | An invite link on the clipboard does not join a game. |

### The settings overlay

F10 (or ⌘, on a Mac keyboard) opens a panel over the game with the
settings that are changed while playing. F10, ⌘, or Esc closes it (Esc
opens the game's pause menu only while the panel is closed). While it is
open, the game gets no keys, mouse or controller input, and the mouse is
released; the game itself does not pause.

- Up and Down (or the mouse pointer, the mouse wheel, the D-pad) select a
  row. Left and Right (or a click, the D-pad) change it. Enter (or Space, a
  click, A) switches a row that is on or off. A slider can be dragged. B
  closes the panel.
- The right side shows the help of the selected row: the comment of its
  setting in `config.toml`, and its name there.
- Each change applies at once, and is written to `config.toml`. Only the
  line of that setting changes; comments and other lines stay.

| Group | Rows |
| --- | --- |
| Display | Resolution (the F8 values), render scale, fullscreen, vertical sync, frame rate counter, frame interpolation (applies at the next start), direct camera |
| Ray tracing | On, screen rays or off; the view (the F6 values, not saved); the traced light (`traced`, `black`, `path`, `off`); the lights (`traced`, `game`); the object shapes (F4); the objects in the rays; the strengths of the occlusion, reflections, bounce, shadows, sun, bounced light, glowing surfaces and lights; the split view |
| Sound | Sound on or off (a game started without sound gets it at the next start), volume |
| Mouse | Sensitivity, invert, aim assist |
| Game | A map (the campaign levels and multiplayer maps found in `maps/`) and Load, which starts it now; look for updates (applies at the next start) |

A change to the traced light, the lights, the object shapes or the objects
starts the traced light's accumulation again: the picture is noisy for a
moment, as at the start of a level.

For tests, `debug.settings_script` (`HALO_SETTINGS_SCRIPT`) presses the
panel's keys at given times, for example
`--settings-script "5=open;6=down,down,right;8=close"`; with
`HALO_SCREENSHOT_DIR` and `HALO_SCREENSHOT_EVERY` the saved frames show the
panel as the window does.

## Multiplayer

### Local network

Start the game on each machine. One machine creates a system link game. The
other machines see the game in the list of system link games.

macOS asks one time for permission to use the local network. Select
"Allow".

### The internet, with forwarded ports

A machine can join a game on the internet when the host forwards ports on
its router:

1. The host forwards TCP port 5150 and UDP port 5150 to its computer, and
   creates a system link game.
2. The other machine sets `network.broadcast = "<host's internet address>"`
   in `config.toml`, starts the game and opens the list of system link
   games.

The search goes to the host. The host answers each machine outside its
local network that searched in the last minute, and the other machine
connects to the address that the answer came from. If the other machine's
router changes the ports of its connections, that machine also forwards UDP
port 5151.

### Internet play

Invite links (`halo://join/...`) operate as on Linux. Refer to "Internet
play" in [port/linux/README.md](../linux/README.md). The host's game puts
its link on the clipboard and in `debug.txt`. To join on macOS, give the
link to the other player. Clicking it opens `Halo.app` (which registers
`halo://`) and joins the game, whether the game runs already or not. Or
set `network.join_from_clipboard = true`, copy the link, and switch to the
game.

## Ray-traced lighting

After the game draws the solid parts of the 3D world, and before the
transparent parts, the fog, the effects and the HUD, the lighting sends
rays from each pixel:

- Ambient occlusion: rays go across the half sphere above the surface. A
  ray that hits a surface near it makes the pixel darker. Corners, creases
  and the ground below objects get darker, as in the real world.
- Reflections: a ray goes in the mirror direction of the view. The surface
  reflects the color where the ray hits, more at glancing angles (the
  Fresnel effect).
- One bounce of indirect light: the color that a screen ray hits adds a
  small quantity of light. A red wall makes the floor next to it a little
  red.
- Sun shadows (Metal only): the sun is the first light of the level's sky
  that has a lens flare, taken as a point light very far away. A ray goes
  towards it from each pixel of an object (the marines, the vehicles, the
  weapon in your hands); if the level is in the way, the pixel is in
  shadow. The level's own surfaces already have the sun's shadows in their
  lightmaps, so their pixels send no shadow ray. The objects' pixels are
  those whose depth has not changed since the game drew the objects,
  before the level (`halo_ray_traced_light_stage(2)`).

The game's lights are traced (`halo_ray_tracing_lights` in
`source/objects/object_lights.c`; `display.ray_tracing_lights`, "traced"
by default): the flashlight, the plasma bolts', the explosions', and the
lights the game only ever drew on the objects, never on the level - Guilty
Spark's, a Covenant lamp's. From each pixel of the level a ray goes to each
light that reaches it (inside its cone, with a soft edge, for a spot like
the flashlight); where the level and the objects do not block it, the
light arrives in its colour, fading with its distance and its angle. The
game's own drawing of its dynamic lights on the level (no shadows, added
onto the lightmaps' light: its share of a pixel's light, the stages above)
is taken out, and the traced light is added in its place. The objects keep
the game's lighting of them, which has all its lights. With "game", the
game draws its dynamic lights and the rays only darken them where they are
blocked.

The glowing things the game draws without a light of their own - a
needle, a plasma bolt's glow, a glowing panel: any object with a light
volume attached (or as a widget) and no light - are lights in the rays (`halo_ray_tracing_emitters` in
`port/linux/game/raytrace_world.c`): each lights what is near it in its
glow's colour, with its shadows. The ray probe draws the rays to them pink.

The characters and the vehicles are in Metal's rays too
(`halo_ray_tracing_objects` in `port/linux/game/raytrace_world.c`): by
default as their drawn models, skinned each frame as the renderer skins
them (each vertex by its two nodes' poses), at the high level of detail,
and traced from both sides; F4 / ⌘J (or `display.ray_tracing_shapes`)
switches to their collision models - the meshes the game tests its bullets
against, a
mesh for each node's region as it is now (its damage permutation), placed
each frame by the node's matrix as the animation poses it. A drawn model
that cannot be read is its collision model; one with neither is left out. Each object is a mesh of its own beside the
level's, in a scene rebuilt each frame, and the rays choose what they see
by the instances' masks:

| Rays | See |
| --- | --- |
| Occlusion from the level | the objects only, near them (the lightmaps have the level's own, baked) |
| Occlusion from an object | the level, the objects |
| The sun from an object | the level, the objects |
| The sun from the level | your body only: the lightmaps have the level's shadows, and the game draws the other objects' |
| Reflections | the level |

So the ground darkens under the marines and the vehicles, and in the sun
you see your own shadow, which the game never drew in the first person.

### What the rays see, and what they do not yet

| Thing | In the rays as | Casts shadows | Lights |
| --- | --- | --- | --- |
| The level | its drawn triangles, with each surface's colour, the light it gives off and its lightmap (`display.ray_tracing_level`: or its collision mesh) | yes | its glowing surfaces (lamps, panels) |
| Characters, vehicles, weapons, items, scenery, devices | their drawn models (or collision models: F4) | yes | their light volumes |
| Projectiles and grenades | their drawn models | yes | their glows (a needle's pink) |
| Your body | its drawn model | the sun's only (the flashlight is in it) | - |
| The sun and the sky | the sky tag's lights: the sun far away, its wide lights (the sky's dome) | - | traced, with their shadows |
| The game's lights (flashlight, plasma, explosions, Guilty Spark, lamps) | point lights and spots, the 16 nearest, reaching 25 units at most | - | yes, traced, in their colour |
| Glows (light volumes) with no light | point lights, up to 16, reaching 12 units at most | - | yes |
| The lightmaps (the level's baked light) | replaced by the traced light (`display.ray_tracing_gi` "traced"); where a bounce lands, its light is the lightmap's there | - | - |
| Water, glass | not in the rays (drawn after them); the water draws its own sky reflection again | no | no |
| Particles, decals, contrails, the sky | no | no | no |
| Shaders' own glow (shields, panels' self-illumination) without a light volume | no | - | no |
| Reflections of the objects | no: reflections see the level only | - | - |
| The objects' lighting | the game's (sampled from the lightmap under each), with the sun's and the level's traced shadows, occlusion and the traced lights | - | - |

A governor keeps the rays off the whole machine's back: a GPU busy for
long enough freezes the Mac's display, not only the game. It reads each
frame's time the rays take on the GPU; over 22 ms it steps up - first the
traced light's new samples thin (every 8th, then 16th frame a pixel), then
the traced lights and emitters halve - under 16 ms for a second it steps
back, and after ten frames in a row over a quarter of a second it stops the
rays and logs why.

### The traced light, in place of the lightmaps

`display.ray_tracing_gi` ("traced" by default) lights the level with the
rays instead of its lightmaps. Halo's lightmaps were baked offline from the
level's own lights: its shaders' radiosity (every lamp, light strip and
glowing panel is a surface that gives off light, in a colour, with a
power) and its sky's lights (the sun, and wide lights for the sky's dome).
The rays read the same data from the map and light each of the level's
pixels themselves:

- The level is in the rays as it is drawn (`halo_ray_tracing_level` in
  `port/linux/game/raytrace_world.c`): each lightmap material's triangles,
  with their lightmap coordinates, each material's colour (its base map's
  average, read by the game's own `bitmap_2d_get_pixel` once the texture
  cache has it), the light it gives off, and its lightmap page (decoded as
  the texture cache loads it, packed into one Metal texture).
- The sun, with its shadow, every pixel every frame.
- The sky's wide lights, a ray toward a point of one; a bounce ray, over
  the half sphere; and a ray to a point of a glowing triangle, chosen as
  likely as the light it gives off (next event estimation). Where a bounce
  lands, the light there is that surface's lightmap times its colour: the
  lightmap already holds every bounce the light took, so one ray brings
  them all. These are new every 4th frame a pixel and accumulate over the
  frames, followed as the camera moves.
- The traced lights and glows (the flashlight, plasma, Guilty Spark's).

The light goes into the game's light buffer - after its lightmap and
dynamic lights pass, before the textures multiply in - on the level's
pixels, so the game's own textures, detail maps and decals shade it as
they shaded the lightmaps. (It is the last frame's rays, each pixel's
point found in their view: Metal's writes of this frame are not yet seen by
GL that early.) The objects, drawn before the level, keep the game's light
with the traced shadows.

"black" leaves the lightmaps out entirely, even where the bounces land:
only what the rays find lit - the sun, the sky, the lamps, the lights -
lights the level. `display.ray_tracing_gi_split` shows the game's light on
the left half of the screen and the traced on the right. The parts'
strengths are `display.ray_tracing_gi_sun`, `_bounce`, `_glow` and
`_lights`.

The traced light costs about 5 ms a frame of the GPU on an M2 Pro (1440 x
1080; the rays at half that). The cube maps (the environment's shiny
reflections) and the objects' own lighting are still the game's.

Each object's mesh is rebuilt each frame, and only the objects within 25
world units of the camera (at most 32) are in. Rays that can find only the
level (the reflections, and the occlusion away from every object's bounding
sphere) go through the level's own structure, not the scene's instances: on
M1 and M2 the rays walk the instances in compute, and it costs every ray. On
The Silent Cartographer's beach, with a dozen units near, the objects cost
about 4.5 ms a frame at 2560x1920 on an M2 Pro; `display.ray_tracing_objects
= false` leaves them out.

The occlusion and the sun's shadows darken the level's baked light (its
lightmaps), not the flashlight's, the plasma's or the other dynamic
lights': the game draws the lightmaps' light, adds the dynamic lights, then
multiplies in the textures, and the lighting takes the light before and
after the dynamic lights (`halo_ray_traced_light_stage` in
`source/render/render.c`) to know each pixel's baked share.

The rays are traced at half the resolution (a quarter of the pixels), then
a blur that stops at edges in depth brings them to the full resolution.
Metal traces 4 occlusion rays per pixel, on a pattern that changes every
pixel of a 4x4 block, so the blur averages 64 directions. The reflection
ray is left out where the surface faces the camera: there it reflects 4%
of the light (Fresnel's), too little to see.

GL and Metal take turns on the GPU (`EGL_ANGLE_metal_shared_event_sync`):
the CPU does not wait for either, and prepares the next frame while the GPU
traces. `HALO_RT_CPU_SYNC=1` makes the CPU wait instead, to compare.

On macOS the rays go through the level itself with Metal's ray tracing
(`port/macos/host/host_metal_rt.m`): the level's collision surfaces
(`port/linux/game/raytrace_world.c`) are a Metal acceleration structure,
built when the level loads. These rays find the level's geometry also where
the camera does not see it. The triangles face out of the level (the
collision surface's plane gives the side), and the rays pass through their
backs: where the drawn surface is a little inside its collision surface, a
ray that leaves the drawn surface does not hit the collision surface behind
it and darken the pixel. A reflection takes its color from the screen
where the camera sees the point that the ray hit. On M1 and M2, Metal traces
the rays in compute; on M3 and later (the M5 and M6 too), in the GPU's ray
tracing hardware.

Rays through the screen's depth buffer also operate on every platform. They
find the objects (the level's surfaces do not include them) and give the
bounce. Where Metal cannot trace rays, they are the only rays.

| `display.ray_tracing` | Rays |
| --- | --- |
| `"on"` | Metal's through the level, and the screen's. |
| `"screen"` | The screen's only. |
| `"off"` | None. |
| `"occlusion"`, `"depth"` | Show what the lighting uses. |
| `"rays"`, `"split"` | The ray view: what Metal's rays find from the camera (`"split"`: the lighting on the left, the ray view on the right). |

The ray view (F6 / ⌘B) traces a ray from the camera through each pixel
into Metal's scene and draws what it finds: the level's collision triangles,
each its own colour with its edges drawn; the characters' and vehicles'
shapes orange; your own body cyan (look down); and, darker, where a second
ray from the hit to the sun is blocked. It is the scene the lighting's rays
go through.

The game's own debug cameras fly round the scene too: ⌘X goes from the
first person to a flying camera, then to one following the player, and
back, one camera a press. In the flying camera, ⌘Z takes its controls:
WASD moves, the mouse turns (as it aims, with `input.mouse_sensitivity`
and `input.invert_mouse`), Space (or the left mouse button) rises, left
ctrl or C (or the right mouse button, or G) sinks, shift or Q held goes four
times as fast, and the arrows up and down change the speed for good; the
player neither jumps, crouches nor lights the flashlight meanwhile. ⌘Z
again lets go of them, and the player moves and aims again while the camera
stays where it is. On the keyboard only ⌘X and ⌘Z do this: X and Z are the
grenade and the zoom. A controller does it too: its black button held for a
second is the next camera, the right stick's click the controls, the
triggers or A and the left stick's click rise and sink, and the white
button speeds up.

The ray probe (F5 / ⌘L) draws the rays the lighting sends from the surface
at the crosshair, as the kernel traced them: the occlusion rays white, the
ray to the sun yellow, the reflection cyan, the rays to the dynamic lights
orange, each red where it hit something; the surface's normal green. Pressed again, the rays stay where
they were, and you can walk round them; lines behind the scene are faint.

`port/macos/tests/run_raytrace_test.sh` draws a test scene through the
lighting on ANGLE, with Metal's rays, as the game draws its frame (the
light, the dynamic lights, the textures), and writes the pictures to
`build/macos/raytrace_test`: bumpy ground, walls, an overhang, a crate, two
marines (objects, not in the level's rays), a flashlight, a plasma light,
and the sun from four directions. `RT_BENCH=200` times the frame with and
without the lighting; `RT_DENSE=1` makes the level about 100,000
triangles; `RT_MODE=screen` leaves out Metal's rays.

## How the port operates

### The guest and the host

The game's data contains 32-bit pointers, so the game must operate with
32-bit pointers (refer to "ILP32 code" in
[port/android/README.md](../android/README.md)). The macOS port uses the
design of the Android port: the game ("the guest") is ILP32 code in a
static ELF image, and a 64-bit program ("the host",
[host/](host)) loads it and does its system calls, its SDL calls and its
OpenGL calls.

An arm64 macOS process cannot map memory below 4 GB (its `__PAGEZERO`
covers the low 4 GB), and arm64_32 code keeps its addresses below 4 GB. The
two builds solve this in different ways:

| Build | Guest code | Guest memory |
| --- | --- | --- |
| `macos` (native) | arm64_32, as on Android, with its memory accesses rebased (below) | A 4 GB region at any 4 GB-aligned address |
| `macos_x86_64` | x32 (x86-64 instructions, 32-bit pointers) | The low 4 GB: the host has a 64 KB `__PAGEZERO` |

### Rebasing (the native build)

`tools/macos_arm64_rebase.py` changes the guest's assembly: each memory
access and each indirect branch goes through `x28`, the base of the guest's
region, plus the low 32 bits of the address register:

```
ldr w0, [x1, #8]   ->   add x27, x28, w1, uxtw
                        ldr w0, [x27, #8]
```

The guest is compiled with `-ffixed-x27 -ffixed-x28`, so the compiler does
not use the two registers. A register contains a guest address (a 32-bit
pointer) or an address in the region (from `sp` or `adrp`). The low 32 bits
of both are the guest address, so the one formula is correct for both.
Accesses through `sp` and `x29` do not change: the guest's stacks are in
the region.

The functions of the host that the guest calls get thunks
(`tools/macos_host_thunks.py`, `tools/android_gl_stubs.py --host-thunks`).
A thunk adds the base of the region to each pointer argument. A host
pointer that the guest keeps (a directory of `posix_directory_open`) goes
to the guest as a small handle.

### The host

| File | Function |
| --- | --- |
| `host_main.c` | Finds the folders, loads ANGLE and the image, runs the game's `main()` on the main thread (Cocoa needs it) on a stack in guest memory. |
| `host_memory.c` | The guest's region, the Xbox memory window, `mmap` for the guest, and the write tracking of the texture cache. |
| `host_loader.c` | Loads the ELF image and fills its import table. |
| `host_thread.c` | Threads with stacks in guest memory, and the calls from the host into the guest. |
| `host_syscall.c` | The guest's Linux system calls on macOS: the numbers, flags, error codes, `stat` and `dirent`, and futexes on `os_sync_wait_on_address`. |
| `host_sdl.c`, `host_gl.c` | SDL3 and OpenGL ES for the guest. |

Apple silicon has 16 KB pages, and the Xbox had 4 KB pages. The game puts
blocks at fixed 4 KB-aligned addresses in the Xbox memory window, so the
window stays mapped, and the guest's `mmap` and `munmap` in it only clear
the memory.

### Game source changes

`HALO_ANDROID` marked all the code of the Android port. It is now three
macros (`port/linux/include/halo_linux_prefix.h`):

- `HALO_GUEST`: the ILP32 guest (Android and macOS), for example the
  stack walker and the true signature of `player_effect_screen_fade_in`.
- `HALO_GLES`: the OpenGL ES renderer (Android and macOS).
- `HALO_ANDROID`: only the Android app (its storage, touch, toasts).

The macOS guest is compiled with `HALO_MACOS`, so it uses the desktop's
code for the window, the mouse, the keyboard and the first start.
`source/render/render.c` calls the ray-traced lighting.

## Tests

- `ninja macos_test` (and `ninja macos_x86_64_test`) builds
  [tests/guest_runtime_test.c](tests/guest_runtime_test.c) as a guest image
  with the host. Start it with `build/macos/test/Halo/halo`. It tests musl,
  threads and futexes, thread-local storage, files and the host's
  directory handles, time, the rebased code and sockets. `port/macos/tests/run_guest_tests.sh` does both steps.
- `port/macos/tests/run_raytrace_test.sh`: refer to "Ray-traced lighting".
- `port/macos/tests/run_determinism_test.sh` runs the game's matrix maths
  and `halo_` functions over 1.4 million inputs on the native and the
  x86-64 builds and compares hashes of the results. Machines in a system
  link game must compute alike, so an optimisation (compiler flags, SIMD)
  must keep these hashes.

## Performance

- Two builds: `ninja macos_app` makes `build/macos/Halo.app`, with the
  original's debug checks (assertions: the game stops at the one that
  fails and says where), for finding bugs; `ninja macos_release_app` makes
  `build/macos-release/Halo.app` without them (`HALO_RELEASE`), for playing.
  Both use the same data, saves and settings.
  `APP=build/macos-release/Halo.app port/macos/sign_app.sh` signs the
  release build.

- The native guest is compiled for the M1 (`-mcpu=apple-m1`), without
  fused multiply-add and without `-ffast-math`, so the results are those
  of the other builds (refer to "Tests").
- `HALO_PROFILE=1 build/macos/Halo/halo` samples every thread 1000 times a
  second, and writes `profile.txt` to the game's folder at exit: the game
  functions and the host functions (SDL, ANGLE) where the time goes.
- `HALO_FPS=1` writes to `host.txt` every 5 seconds: the frames per
  second, the time the main thread waits for the GPU to finish the GL
  frame, and the time Metal's rays take.
- `display.render_scale = 0.75` (or F8 / ⌘R to 1440p or 1080p) is the
  largest speed-up with ray tracing on a Retina display: the rays' cost
  follows the number of pixels.

## Find problems

- `HALO_MAP=b30` starts a map without the menus: a campaign level's name
  (`a10` to `d40`), a multiplayer map's (`bloodgulch`) or a scenario path.
- `HALO_TEST_INPUT="script:47=switch,50=zoom,52-60=turnright"` plays the
  scripted actions in those seconds since start (forward, back, left, right,
  turnleft, turnright, up, down, fire, grenade, jump, crouch, zoom, action,
  flashlight, reload, switch, black, start; `camera` and `cameracontrol`
  press ⌘X and ⌘Z once, `fast` holds shift, `mouseleft`, `mouseright`,
  `mouseup` and `mousedown` move the mouse 400 pixels a second); with
  `HALO_SCREENSHOT_DIR` and
  `HALO_SCREENSHOT_EVERY=<frames>` it records a test drive, and
  `HALO_EXIT_AFTER=<seconds>` ends it.
- `HALO_COMMANDS="47=cheat_all_weapons;50=cheat_spawn_warthog"` runs
  console commands at those seconds since start: with the scripted input,
  a test drive can stage a scene (weapons, vehicles, grenades).

- `host.txt` in the game's folder is the log of the host, and, when the
  game does not start from a terminal, of the game's port (for one, why
  it quit: `window closed` is ⌘Q or the window's close button). `debug.txt`
  is the log of the game. `host.old.txt` is the run before's `host.txt`. Start `halo` in a terminal to see both.
- If the guest code stops, `host.txt` shows the registers and the frame
  chain. To find the functions, enter
  `llvm-symbolizer --obj=build/macos/Halo/halo_guest.elf <address>` with
  the guest addresses.
- `HALO_HIDDEN_WINDOW=1 HALO_EXIT_AFTER=5 build/macos/Halo/halo` starts
  the game without a window and stops it after 5 seconds.

## Security

Before this port was made, the repository was examined for code that could
harm a Mac (refer to `SECURITY_AUDIT.md` next to the repository). The
macOS port:

- does not update itself (`port/linux/src/updater.c` is not part of it);
- does not ask the router to forward a port, and does not join games from
  the clipboard, unless `config.toml` says so;
- lets the internet play tunnel give a remote machine's traffic only to the
  ports of the game's own sockets (`p2p_socket_port` in
  `port/linux/src/p2p.c`), not to other
  programs on the Mac.
