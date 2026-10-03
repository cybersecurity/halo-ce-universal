# Distribution status

The Apple applications are unofficial community ports, not Microsoft, Bungie,
or Halo Studios products. This document records packaging decisions, not a
legal opinion or permission to distribute the reconstructed game code.

## What the packages contain

Apple builds embed the compiled native runtime and reconstructed game code,
SDL, ANGLE, bundled room-transport JavaScript, open-source dependency notices,
and an original geometric icon. They do not embed a user's disc image, maps,
saves, or the repository's replacement HUD/title/font assets. In-game images,
audio and fonts are loaded from the user's imported disc image/map cache.

First launch opens a native Files/Finder picker. Import copies the selected
ISO/XISO into private storage, hashes it with SHA-256 while copying, reads the
copy back to verify the same digest, and checks XDVDFS structure and the
complete supported Xbox Halo map set. It publishes the generation atomically
only after these steps succeed. A cancelled or rejected import leaves the
previous generation and saves intact. Subsequent starts use the retained copy's
extracted map cache, checking the image's size and map headers; they do not
re-hash several gigabytes on every launch. The stored SHA-256 detects copy
errors; it does not prove ownership or authenticate a known retail pressing.
A full disc requires space for the image plus approximately 1.9 GB of maps.

On iOS this storage is inside the app container. On macOS it is in
`~/Library/Containers/org.haloce.macos/Data/Library/Application Support/org.haloce.macos/`
in the sandboxed distribution build (unentitled local builds use the normal
Application Support directory). Saves and settings live
outside the replaceable game generation. Existing installations that only
have extracted maps must select their image once; existing maps/saves are kept.

## Rights and distribution channels

The upstream project labels its contributions CC0, and dependency licenses
must remain included. That label does not establish the contributor's rights
to reconstructed proprietary code, original assets, or trademarks. Requiring
a user-supplied game image does not settle those questions. Microsoft's
[Game Content Usage Rules](https://www.xbox.com/en-US/developers/rules) are not
a clearance for this decompiled port: they expressly restrict reverse
engineering to create covered items. Obtain a qualified IP review or relevant
rights-holder permission before treating a public release as legally cleared.

Mac distribution should use Developer ID signing and Apple notarization;
[Apple's distribution guidance](https://developer.apple.com/developer-id/)
explains the separate Gatekeeper requirements. Signing/notarization is not
copyright clearance. An IPA still needs an appropriate Apple-authorized
signing/distribution method; an unsigned IPA is not universally installable.
