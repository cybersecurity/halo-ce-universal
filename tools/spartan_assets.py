#!/usr/bin/env python3
"""Makes high-res textures of the profile screens' Spartan pictures
(port/assets/spartans/*.png) from the larger pictures of the same Spartan
in the maps:

    python tools/spartan_assets.py layout --map assets/maps/ui.map
    python tools/spartan_assets.py build --map assets/maps/ui.map
    python tools/spartan_assets.py check --map assets/maps/ui.map --out /tmp/spartan_check

The profile screens (SELECT PROFILE, the cooperative and the 4-way split
screen and System Link ones) draw a picture of each profile's Spartan in its
colour: a frame of ui.map's colors_sm (128x256, its art in the top left
corner, drawn at one texel to the pixel by the widget's background), one for
each colour and one for an empty profile. The MULTIPLAYER COLOR screen draws
the same renders larger, from player_color_marine_large (256x512), which
colors_sm's are a reduction of: frame for frame, at 0.47 of their size. So a
texture of twice colors_sm's size, in its layout, made from the larger
render, draws in its place unchanged, as the high-res HUD's do
(port/linux/src/hud_hires.c), and with the high-res text
(display.high_res_text), as the menus' titles do. Only the larger render's
own texels are used: reduced (premultiplied, with a triangle filter) to
where colors_sm has them, nothing sharpened or drawn. The empty profile's
frame is other art in each, so it keeps the map's.

layout: reads both groups from the map and finds the reduction of the
    larger frames that covers colors_sm's best (their scale, and the move in
    colors_sm's texels), from the frames' alpha. Writes
    port/assets/spartans/spartans.json: that place, and each frame's bitmap
    (size, format and the CRC of its pixels, which the game checks before
    drawing the texture in its place) and the CRC of its larger frame.
build: reduces the larger frames as spartans.json places them into
    port/assets/spartans/*.png (committed; the builds embed them:
    tools/embed_assets.py).
check: compares each PNG, reduced to its bitmap's size, with the map's
    bitmap and writes side-by-side images into --out.

Needs Pillow, NumPy and SciPy.
"""

import argparse
import json
import sys
import zlib
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from hud_assets import FORMATS, XboxMap, bleed, decode_bitmap, level0_size, overlap  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT / "port/assets/spartans"
LIST = ASSETS / "spartans.json"
SCALE = 2

SMALL = "ui\\shell\\bitmaps\\colors_sm"
LARGE = ("ui\\shell\\main_menu\\settings_select\\player_setup\\player_profile_edit\\color_edit\\"
         "player_color_marine_large")
# the colours' frames (the last of the 19, the empty profile's, is other
# art in each group)
COLOURS = 18
# the part of colors_sm's frames with art, which layout compares
ART = (72, 144)


def groups(xbox_map: XboxMap) -> tuple:
    """colors_sm's bitmaps and player_color_marine_large's."""
    result = []
    for tag in (SMALL, LARGE):
        if ("bitm", tag) not in xbox_map.tags:
            sys.exit(f"{tag}: not in this map")
        bitmaps = xbox_map.bitmap_group(tag)["bitmaps"]
        if len(bitmaps) <= COLOURS:
            sys.exit(f"{tag}: {len(bitmaps)} bitmaps, not a frame for each of the {COLOURS} colours")
        result.append(bitmaps)
    return tuple(result)


def reduced(large: np.ndarray, width: int, height: int, place: list) -> np.ndarray:
    """A larger frame as a picture of width x height: scaled by place[0] and
    moved by place[1:] (texels of the picture). (Pillow filters RGBA with its
    colour premultiplied by its alpha.)"""
    scale, x, y = place
    # (transparent around it, as the box may reach past its edges)
    margin = int(np.ceil((max(width, height) + abs(x) + abs(y)) / scale)) + 2
    padded = np.zeros((large.shape[0] + 2 * margin, large.shape[1] + 2 * margin, 4), np.uint8)
    padded[margin:margin + large.shape[0], margin:margin + large.shape[1]] = large
    box = (margin - x / scale, margin - y / scale, margin + (width - x) / scale, margin + (height - y) / scale)
    return np.asarray(Image.fromarray(padded, "RGBA").resize((width, height), Image.BILINEAR, box=box))


def fit(small: list, large: list) -> list:
    """The place of the larger frames whose reduction covers colors_sm's
    frames best: from where their shapes have the same area and centre."""
    from scipy import optimize

    width, height = ART

    def shape(alpha: np.ndarray) -> tuple:
        alpha = alpha.astype(float) / 255
        ys, xs = np.mgrid[0:alpha.shape[0], 0:alpha.shape[1]]
        area = alpha.sum()
        return area, ((xs + 0.5) * alpha).sum() / area, ((ys + 0.5) * alpha).sum() / area

    area, x, y = shape(np.mean([frame[:height, :width, 3] for frame in small], axis=0))
    large_area, large_x, large_y = shape(np.mean([frame[..., 3] for frame in large], axis=0))
    scale = (area / large_area) ** 0.5
    targets = [frame[:height, :width, 3].astype(float) / 255 for frame in small]

    def error(place: np.ndarray) -> float:
        return float(sum(np.abs(reduced(frame, width, height, list(place))[..., 3] / 255 - target).mean()
                         for frame, target in zip(large, targets)) / len(targets))

    start = [scale, x - scale * large_x, y - scale * large_y]
    best = optimize.minimize(error, start, method="Powell", options={"xtol": 1e-3, "ftol": 1e-7},
                             bounds=[(scale * 0.9, scale * 1.1), (start[1] - 4, start[1] + 4),
                                     (start[2] - 4, start[2] + 4)]).x
    return [round(float(best[0]), 3), round(float(best[1]), 2), round(float(best[2]), 2)]


def layout(arguments) -> None:
    small, large = groups(XboxMap(Path(arguments.map)))
    place = fit([decode_bitmap(bitmap) for bitmap in small[:COLOURS]],
                [decode_bitmap(bitmap) for bitmap in large[:COLOURS]])
    print(f"{LARGE}: reduced {place[0]}x, moved {place[1]}, {place[2]}")
    entries = []
    for index, bitmap in enumerate(small[:COLOURS]):
        entries.append({
            "name": SMALL.split("\\")[-1] + f"__{index}",
            "tag": SMALL,
            "bitmap": index,
            "width": bitmap["width"],
            "height": bitmap["height"],
            "format": FORMATS[bitmap["format"]],
            "scale": SCALE,
            # (the map's, which the game checks before drawing this in its
            # place: modified maps may differ)
            "crc": zlib.crc32(bitmap["pixels"][:level0_size(bitmap)]),
            # (the larger frame of the same index it is made from)
            "source_crc": zlib.crc32(large[index]["pixels"][:level0_size(large[index])]),
        })
    LIST.write_text(json.dumps({"source": LARGE, "place": place, "assets": entries}, indent=1) + "\n")


def build(arguments) -> None:
    xbox_map = XboxMap(Path(arguments.map))
    groups(xbox_map)
    description = json.loads(LIST.read_text())
    names = {f"{entry['name']}.png" for entry in description["assets"]}
    for stale in ASSETS.glob("*.png"):
        if stale.name not in names:
            stale.unlink()
    sources = xbox_map.bitmap_group(description["source"])["bitmaps"]
    for entry in description["assets"]:
        source = sources[entry["bitmap"]]
        if zlib.crc32(source["pixels"][:level0_size(source)]) != entry["source_crc"]:
            sys.exit(f"{description['source']} bitmap {entry['bitmap']}: not the one laid out")
        scale = entry["scale"]
        place = [value * scale for value in description["place"]]
        # (the colour carried into the transparent texels, so that filtering
        # and the mip levels keep it at the Spartan's edge)
        image = bleed(reduced(decode_bitmap(source), entry["width"] * scale, entry["height"] * scale, place))
        Image.fromarray(image, "RGBA").save(ASSETS / f"{entry['name']}.png", optimize=True)
        print(f"{entry['name']}.png: {image.shape[1]}x{image.shape[0]}")


def check(arguments) -> None:
    xbox_map = XboxMap(Path(arguments.map))
    output = Path(arguments.out)
    output.mkdir(parents=True, exist_ok=True)
    width, height = ART
    for entry in json.loads(LIST.read_text())["assets"]:
        bitmap = xbox_map.bitmap_group(entry["tag"])["bitmaps"][entry["bitmap"]]
        if zlib.crc32(bitmap["pixels"][:level0_size(bitmap)]) != entry["crc"]:
            print(f"{entry['name']}: this map's bitmap is not the one laid out")
        xbox = decode_bitmap(bitmap)
        image = Image.open(ASSETS / f"{entry['name']}.png").convert("RGBA")
        scale = entry["scale"]
        small = np.asarray(image.resize((entry["width"], entry["height"]), Image.BOX))
        alpha = overlap(small[:height, :width, 3].astype(float), xbox[:height, :width, 3].astype(float))
        colour = np.abs(small[:height, :width, :3].astype(float) * small[:height, :width, 3:] / 255 -
                        xbox[:height, :width, :3].astype(float) * xbox[:height, :width, 3:] / 255).mean()
        print(f"{entry['name']}: alpha overlap {alpha:.3f}, colour difference {colour:.2f} of 255")
        # the map's | ours reduced | ours, over the menus' dark blue
        ours = image.crop((0, 0, width * scale, height * scale))
        panels = [Image.fromarray(np.ascontiguousarray(picture[:height, :width]), "RGBA").resize(
            ours.size, Image.NEAREST) for picture in (xbox, small)]
        panels.append(ours)
        sheet = Image.new("RGB", (ours.width * 3 + 16, ours.height), (255, 0, 0))
        for place, picture in enumerate(panels):
            background = Image.new("RGBA", picture.size, (16, 26, 60, 255))
            sheet.paste(Image.alpha_composite(background, picture).convert("RGB"), (place * (ours.width + 8), 0))
        sheet.save(output / f"{entry['name']}.png")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("layout", "build", "check"):
        command = commands.add_parser(name)
        command.add_argument("--map", required=True)
        if name == "check":
            command.add_argument("--out", required=True)
    arguments = parser.parse_args()
    {"layout": layout, "build": build, "check": check}[arguments.command](arguments)


if __name__ == "__main__":
    sys.exit(main())
