# Xbox v5 community maps

Native Windows, Linux, and Android builds discover compatible Xbox v5
multiplayer caches in the active game-data map directory. Add the `.map` files
alongside the retail maps before starting the game, then open the multiplayer
map selector. If the installation uses a localized `maps_de`, `maps_fr`,
`maps_es`, or `maps_it` directory, use that active directory. Restart after
adding or removing maps: discovery runs once per process.

The original 13 multiplayer maps retain their order, localized names, and
previews. Custom entries follow in case-insensitive name order, with a generic
preview and a community-map caption in both the PC and Xbox menu styles. The
list holds 128 maps total. A custom map uses its cache basename for selection,
saved preferences, and network identity. Keep the same compatible map files on
every player’s machine; map names alone do not establish matching contents.

Discovery checks the Xbox v5 header and footer, multiplayer scenario type,
recognized regional build, bounded tag-data range, and a cache name matching
the filename without `.map`, ignoring case. Names must contain 1–31 ASCII
letters, digits, spaces, underscores, or hyphens. Retail-name duplicates are
not added. Use distinct names for community variants, such as `h1pb_prisoner`,
and preserve the original retail files. Matching header names require a cache
built with that identity; renaming a file alone is insufficient.

The native multiplayer disk-cache limit is 128 MiB of declared uncompressed
data, including the exact boundary. The Xbox tag arena remains 22 MiB. Direct
precaching also enforces the multiplayer disk limit. These are header and
capacity checks, not complete validation of a map’s payload, tags, scripts,
game modes, or behavior.

These files must already be in Xbox cache version 5. Halo PC v7 and Custom
Edition v609 files are different formats; renaming them or changing the version
field does not convert them. Source-tag conversion and game data are not
included in this change.

Focused checks compile the production scanner, cache capacity checks, and both
menu adapters with synthetic maps and AddressSanitizer/UndefinedBehaviorSanitizer:

```sh
python3 -m unittest tools.test_custom_maps -v
```
