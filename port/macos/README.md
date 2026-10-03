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

The Debug menu contains MetalFX upscaling, shader-stall recording, clearing
recorded events, and AirDrop export. MetalFX and recording settings persist.
Shader capture intentionally synchronizes the GPU and can reduce performance.

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
