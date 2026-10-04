# Native Apple Silicon Mac app

Build with `python3 tools/macos_build.py` on an Apple Silicon Mac with Xcode,
Homebrew LLVM and LLD. This shares the iOS signed ARM64 ILP32 guest, address
arena and ABI bridges, with an AppKit host, SDL keyboard/mouse/gamepads,
ANGLE Metal rendering, and the same bundled PR #12 room transport. No touch
overlay is created on macOS. OpenGL ES remains an iOS fallback; this Mac
package currently ships the Metal backend only.

First launch uses a Finder file picker to import a user-owned original Xbox
PAL or NTSC-US ISO/XISO. A retained private image and map cache are published
only after readback SHA-256 verification and map validation. See
[distribution notes](../apple/DISTRIBUTION.md) for storage paths, packaging,
and unresolved legal clearance.

Choose **Settings…** from the application menu (⌘,) to open a separate
settings window with MetalFX upscaling, shader-stall recording, clearing
recorded events, and AirDrop export. iOS continues to present its settings
as a modal when the phone is shaken. MetalFX and recording settings persist.
Shader capture intentionally synchronizes the GPU and can reduce performance.

Native menu tracking pauses the frame loop. An event-tracking timer services
sound streaming (including DirectSound completions) on the guest main thread
while inside the event pump, allowing queued music to refill without calling game callbacks
from the audio worker. `HALO_MAC_TEST_MENU_AUDIO=1` opens a native test menu
for eight seconds and traces PCM output once per second.

Package the app with:

```
python3 tools/macos_package.py --identity 'Developer ID Application: YOUR NAME (TEAM)'
```

The script signs embedded frameworks and the sandboxed app with hardened
runtime, verifies signatures, and creates a signed DMG. Notarization is a
separate step using your Apple credentials; the script does not claim that
its output is notarized or legally cleared for public distribution.

Validated locally: native Metal main menu and Silent Cartographer rendering,
audio, private-image transaction tests, and signed sandboxed runtime launch.
Mac controller hardware, Mac/browser crossplay and extended MetalFX behavior
still need interactive testing. iOS/browser crossplay was validated previously;
sharing that transport does not establish that every Mac network path works.
