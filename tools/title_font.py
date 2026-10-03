#!/usr/bin/env python3
"""Builds OpenCE (Open Community Edition), the typeface of the menus' titles
(port/assets/fonts):

    python tools/title_font.py             # port/assets/fonts/OpenCE-Regular.ttf
    python tools/title_font.py --measure   # its spacing, measured from titles.json

The maps' titles are set in a commercial typeface, which cannot be shipped.
OpenCE is Newtown (Roger White, 1994), a typeface in the same style that its
author released into the public domain (port/assets/fonts/Newtown-LICENSE.txt),
respaced to set the titles as the maps do. Only its spacing is changed: its
outlines are Newtown's (port/assets/fonts/Newtown-Regular.ttf, unmodified),
the glyphs below only moved sideways in their boxes.

The spacing is measured from the maps' title pictures: tools/title_assets.py
places each letter where it covers the old one best and records where
(port/assets/titles/titles.json), and --measure solves, by least squares over
the letter pairs, the space beside each letter (and a spacing for each title,
which the titles were set with by hand), the pairs that still differ by 10
units or more becoming kerning pairs. Its values are pasted into SPACING,
SPACE and KERNING. Newtown's own kerning pairs between letters left as they
were are kept; its pairs with a respaced letter are replaced by the measured
ones.

OpenCE is under the SIL Open Font License 1.1 (OpenCE-OFL.txt). Needs
fontTools (and NumPy for --measure).
"""

import sys
from pathlib import Path

from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "port/assets/fonts/Newtown-Regular.ttf"
OUTPUT = ROOT / "port/assets/fonts/OpenCE-Regular.ttf"
TITLES = ROOT / "port/assets/titles/titles.json"
FAMILY = "OpenCE"
VERSION = "2.000"
COPYRIGHT = ("OpenCE: Newtown, by Roger White (1994), released by its author into the public domain; "
             "respaced by The OpenCE Project Authors (2026)")

# the space beside each respaced character (left, right of its ink), in font
# units (1000 to the em, capitals 719 tall)
SPACING = {
    "1": (60, 71), "2": (25, 34), ":": (26, 58), "A": (-31, -29), "C": (35, 25), "D": (68, 26),
    "E": (29, 46), "F": (57, 29), "G": (24, 46), "H": (59, 50), "I": (68, 68), "K": (74, -31),
    "L": (56, -2), "M": (69, 63), "N": (67, 69), "O": (22, 24), "P": (72, 24), "R": (67, 10),
    "S": (28, 24), "T": (2, -12), "U": (58, 67), "V": (-36, -27), "Y": (-42, -43),
}

# the width of a space
SPACE = 379

# pairs set closer (negative) or further apart than their spacing gives
KERNING = {
    "AP": -12, "CU": 18, "MA": 14, "MS": 11, "NL": -14, "NT": -10, "OA": 11, "OO": 36,
    "OS": -12,
}


def ink(font: TTFont, character: str) -> tuple:
    """A character's glyph name and its outline's left and right."""
    name = font.getBestCmap()[ord(character)]
    glyph = font["glyf"][name]
    return name, glyph.xMin, glyph.xMax


def build(output: Path = OUTPUT) -> Path:
    font = TTFont(SOURCE)
    glyf, hmtx, cmap = font["glyf"], font["hmtx"], font.getBestCmap()
    respaced = set()
    for character, (left, right) in SPACING.items():
        name, x_min, x_max = ink(font, character)
        glyph = glyf[name]
        if left != x_min:
            glyph.coordinates.translate((left - x_min, 0))
            glyph.recalcBounds(glyf)
        hmtx[name] = (left + (x_max - x_min) + right, left)
        respaced.add(name)
    space = cmap[ord(" ")]
    hmtx[space] = (SPACE, hmtx[space][1])
    table = font["kern"].kernTables[0]
    pairs = {pair: value for pair, value in table.kernTable.items()
             if pair[0] not in respaced and pair[1] not in respaced}
    for pair, value in KERNING.items():
        pairs[cmap[ord(pair[0])], cmap[ord(pair[1])]] = value
    table.kernTable = pairs
    names = {
        0: COPYRIGHT,
        1: FAMILY,
        2: "Regular",
        3: f"{FAMILY}-Regular;{VERSION}",
        4: f"{FAMILY} Regular",
        5: f"Version {VERSION}",
        6: f"{FAMILY}-Regular",
        13: "This Font Software is licensed under the SIL Open Font License, Version 1.1.",
        14: "https://openfontlicense.org",
    }
    table = font["name"]
    for name_id, text in names.items():
        table.removeNames(nameID=name_id)
        table.setName(text, name_id, 3, 1, 0x409)
        table.setName(text, name_id, 1, 0, 0)
    font["head"].fontRevision = float(VERSION)
    font.save(output)
    return output


def measure():
    """SPACING, KERNING and SPACE as the maps' titles place the letters
    (titles.json), printed to be pasted above."""
    import json

    import numpy as np

    font = TTFont(SOURCE)
    cap = font["glyf"][font.getBestCmap()[ord("H")]].yMax
    observations, seen = [], set()
    for title in json.loads(TITLES.read_text())["assets"]:
        if "in_place_of" in title:
            continue    # (a text set with the font's spacing, not as the map's: QUIT)
        if (title["tag"], title["text"]) in seen:
            continue    # (a selected item's text is placed as its other frame's)
        seen.add((title["tag"], title["text"]))
        units = cap / title["cap"]
        letters, spaced, space = [], [], False
        for character in title["text"]:
            if character == " ":
                space = True
                continue
            letters.append(character)
            spaced.append(space)
            space = False
        for index in range(len(letters) - 1):
            a, b = letters[index], letters[index + 1]
            _, x_min, x_max = ink(font, a)
            gap = (title["lefts"][index + 1] - title["lefts"][index]) * units - (x_max - x_min)
            observations.append((title["name"], a, b, spaced[index + 1], gap))
    characters = sorted({c for o in observations for c in o[1:3]})
    column = {}
    for c in characters:
        column["left", c], column["right", c] = len(column), len(column) + 1
    column["space"] = len(column)
    for name in sorted({o[0] for o in observations}):
        column["title", name] = len(column)
    rows, values = [], []
    for name, a, b, space, gap in observations:
        row = np.zeros(len(column))
        row[column["right", a]] += 1
        row[column["left", b]] += 1
        row[column["title", name]] += 2 if space else 1
        if space:
            row[column["space"]] += 1
        rows.append(row)
        values.append(gap)
    # (the titles' own spacing averages nothing; the letters' left and right
    # spaces balance as Newtown's do)
    rows.append(10 * np.array([1.0 if isinstance(key, tuple) and key[0] == "title" else 0.0 for key in column]))
    values.append(0)
    row = np.zeros(len(column))
    balance = 0
    for c in characters:
        name, x_min, x_max = ink(font, c)
        row[column["left", c]], row[column["right", c]] = 1, -1
        balance += x_min - (font["hmtx"][name][0] - x_max)
    rows.append(10 * row)
    values.append(10 * balance)
    solution, *_ = np.linalg.lstsq(np.array(rows), np.array(values), rcond=None)
    residuals = {}
    for (name, a, b, space, gap), row in zip(observations, rows):
        if not space:
            residuals.setdefault(a + b, []).append(gap - row @ solution)
    kerning = {pair: round(float(np.median(r))) for pair, r in sorted(residuals.items()) if abs(np.median(r)) >= 10}
    spacing = {c: (round(float(solution[column["left", c]])), round(float(solution[column["right", c]])))
               for c in characters}
    print("SPACE =", round(float(solution[column["space"]])))
    print("SPACING =", spacing)
    print("KERNING =", kerning)


if __name__ == "__main__":
    if sys.argv[1:] == ["--measure"]:
        measure()
    else:
        print(build(Path(sys.argv[1]) if len(sys.argv) > 1 else OUTPUT))
