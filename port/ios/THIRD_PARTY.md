# Credits and third-party notices

This iOS port builds on [cybersecurity/halo-ce-universal](https://github.com/cybersecurity/halo-ce-universal),
the native ports in [bnunu/halo-1](https://github.com/bnunu/halo-1), and the
decompilation work in [punpckhdq/halo](https://github.com/punpckhdq/halo).
The inherited project license is [CC0 1.0](../../LICENSE.md).

The app includes SDL 3.4.16 (zlib license), portions of musl 1.2.5 (see its
COPYRIGHT for the MIT license and component notices), KCP (MIT), and tomlc17
(MIT). The native XISO parser adapts upstream extraction code following
extract-xiso (modified BSD; see `port/third_party/extract-xiso/LICENSE.TXT`).
This product includes software developed by in <in@fishtank.com>. Their complete notices are copied into the built app's `Licenses`
directory. Khronos headers retain the notices supplied by their registries.
Other vendored components retain their original notices in the source tree.

Halo, Master Chief, game artwork, and original game assets belong to their
respective rights holders. The project license does not grant rights to those
materials. The icon adapts supplied Halo artwork using image generation; see
[ICON.md](ICON.md). No game maps, disc images, Microsoft signing material, or
Xbox SDK are distributed in the app. This is an unofficial community port,
unaffiliated with Microsoft, Bungie, or Halo Studios.

## ANGLE Metal backend

The Metal renderer bundles [ANGLE](https://github.com/google/angle) at
`c053bf85793bbb83016b1196d04e5df3594b9bcc` (BSD 3-clause), using the
`v2.1.28252` iOS/device and simulator frameworks published by
[EdgeFirstAI/angle-package](https://github.com/EdgeFirstAI/angle-package).
The download is pinned by SHA-256 in `tools/ios_angle.py`. ANGLE and translator
dependency notices (xxHash, ceval, glslang, SPIR-V headers/tools/cross, Abseil,
and zlib) are copied into the app's `Licenses` directory. ANGLE receives only
native graphics calls; it is not a browser or a WASM runtime.
