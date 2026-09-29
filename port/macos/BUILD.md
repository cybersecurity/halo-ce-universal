# Apple Silicon source build

This experimental target builds from the current checkout, including indexed working changes; it never fetches an older copy of this repository or overwrites its source with a Mac overlay. Stage newly added source inputs first. The standalone public tooling has completed its pinned-source native build and static bundle checks. Runtime acceptance exercised bundled-data startup, audio, keyboard controls and exact F10 preset cycling on macOS 26.

Install Apple Command Line Tools and Homebrew, then run from the repository root with your own supported retail Xbox XDVDFS disc image:

```bash
./port/macos/setup.sh "/path/to/your/Halo Xbox.iso" --jobs 4
```

The helper installs build dependencies, fetches pinned XboxRecomp/musl dependencies, compiles and translates the game, runs source/native fixtures, and packages a native signed app. All maps, movies and non-system libraries are bundled. It does not download Halo or launch the game. Existing images, preferences and saves are preserved. `HALO_WORK_DIR` and `HALO_APP_OUTPUT` select fresh locations outside this checkout; default output is `~/Applications/Halo Combat Evolved.app`.

Only indexed source bytes enter the isolated build; working edits and deletions remain intact, generated outputs/assets are excluded, and source symlinks are rejected. Provenance records the current commit and relative source hashes. Mac deployment defaults to 14; the package minimum is the maximum of its engine and library deployment requirements. F10 cycles and saves 4:3 (640×480), 720p and 1080p; F11 toggles fullscreen and F12 releases the pointer. Two same-host native instances synchronized movement on Blood Gulch and Battle Creek. Older OS support, physical wireless controllers, full mission completion, cross-platform networking and HD texture packs remain unverified.

Original Mac tooling is MIT; the root source license and third-party notices remain unchanged. See NOTICE.md and tooling/licenses. Build CI uses synthetic headers, never downloads game data and never uploads generated translations or compiled apps.
