# Halo: CE app icon

Imported unchanged from NicholasDominici/halo-ce-ios at `3f2c14101d3ae1c7f0c0a11993a43407fadbeb46`.
The upstream provenance notes below describe its original creation.

Created with the built-in `image_gen` tool from the circular Master Chief icon
provided by the user on 2026-09-28. The selected generated image is preserved
as `Icon-Artwork.png`. `Assets.xcassets/AppIcon.appiconset/AppIcon-1024.png` is
the opaque 1024-pixel source used by the asset catalog; smaller device sizes
are standard Lanczos resizes. iOS applies the rounded-square mask at display
time, so there are no baked-in borders or transparent corners.

## Generation prompt

```text
Use case: precise-object-edit
Asset type: production iOS Home Screen app icon for Halo: CE.
Input image: the supplied circular game icon is the edit target and exact artwork reference.
Task: Adapt this existing artwork to a proper iOS app icon. Remove the circular silver rim and the black area outside the circle. Extend the existing blue sky, streaks of light and green landscape naturally to all four corners of a full-bleed square.
Preserve the same existing green-armored character, gold visor, helmet design, pose, weapon, lighting and early-2000s rendered game-art style. Keep the recognizable composition and character centered. Improve resolution and edge clarity while staying faithful to this image; no new objects or text.
Technical requirements: 1024x1024 opaque square PNG artwork, fills the entire canvas edge to edge. iOS applies its own rounded-square icon mask, so do not bake rounded corners, a circle, or transparent corners into the source. Keep the helmet and other important features inside a roughly 10% safe margin. No border, no ring, no watermark, no letters, no UI mockup. Return only the actual icon artwork.
```
