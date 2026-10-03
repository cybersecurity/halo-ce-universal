#!/usr/bin/env python3
"""Makes high-res textures of the menus' titles (port/assets/titles/*.png):
the screens' headers and the main menu's items, which the maps hold as
pictures of text, set again in a font at 4x their size:

    python tools/title_assets.py --map assets/maps/ui.map

The maps' titles are set in a commercial typeface; they are set here in
OpenCE (port/assets/fonts/OpenCE-Regular.ttf, SIL Open Font License), Roger
White's public-domain Newtown respaced to set them as the maps do
(tools/title_font.py). Each
title keeps the rest of its picture: the text is taken out of the map's
bitmap (filled in from around it), that background (soft plates and glows)
is enlarged, and the text set in the font, in its colour, goes over it.
Where the project has a hand-made SVG redraw of the picture (BACKGROUNDS,
port/assets/titles/svg, needing rsvg-convert), the text goes over the redraw
instead.
Each letter is placed where the old one is (Layout): the text is set with
the font's spacing, moved and tracked as a whole to cover the old text
best, and then each letter is moved by up to four texels, in eighths of a
texel, to cover its old letter best; letters left nearly touching where the
old ones have a gap between them are parted to the old gap. A selected menu
item uses its unselected picture's layout (the same letters, read more
surely). titles.json records where each letter went, which
tools/title_font.py's spacing is measured from. QUIT, which the desktop
builds draw in GAME DEMOS's frames (source/interface/ui_widget.c), is set in
its place with the font's spacing alone, for those frames (VARIANTS), its Q
the O with a short foot as Halo PC's is (FEET).

The pictures are drawn in place of the maps' bitmaps as the high-res HUD's
are (port/linux/src/hud_hires.c), and only for the bitmaps of the English
maps (their CRC), which these texts are. Writes titles.json, which
tools/embed_assets.py embeds with them. Needs Pillow, NumPy and SciPy.
"""

import argparse
import copy
import json
import sys
import zlib
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, str(Path(__file__).resolve().parent))
from hud_assets import FORMATS, XboxMap, decode_bitmap, level0_size, render_svg  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
TITLES = ROOT / "port/assets/titles"
FONT = ROOT / "port/assets/fonts/OpenCE-Regular.ttf"
SCALE = 4
MAXIMUM_SIZE = 2048
# the precision letters are placed with: eighths of the map's texels
SUPER = 8
# how far a letter may be moved from the font's spacing: four texels (and
# never past the middle of its neighbours, so that like letters, as U and L,
# cannot trade places)
MAXIMUM_MOVE = 4 * SUPER
# how far (texels) the gap between two letters is looked for, and how much
# more matching the old letters' gaps counts than covering them best
GAP_SEARCH = 6
GAP_WEIGHT = 4.0
# coverage a texel inside a letter has, and how much narrower than the old
# one a gap may be before the letters are parted
SOLID = 0.9
GAP_TOLERANCE = 0.05
# how narrow (texels) a gap between two letters must be to be matched
GAP_NEAR = 0.5
# how much a plate is blurred (in the map's texels) before it is enlarged
SMOOTHING = 1.0
# how far (texels) around a text set in the map's place the old text is taken
# out: with the halo DXT3 leaves around its letters, which nothing covers then
HALO = 2
# a letter set as another's shape with a foot: QUIT's Q is the O with a short
# foot at its bottom right, as Halo PC's is (OpenCE's Q has a diagonal tail):
# over the last FOOT of the capitals' height, the bowl's right edge runs
# straight down to its bottom instead of curving in
FEET = {"Q": "O"}
FOOT = 0.16

MENU = "ui\\shell\\main_menu\\"
# each title bitmap's text, by bitmap (as the English maps' pictures read)
TEXTS = {
    MENU + "difficulty_select\\header_choose_difficulty": ["CHOOSE DIFFICULTY"],
    MENU + "gametype_select\\header_select_gametype": ["SELECT GAMETYPE"],
    MENU + "menu_game_demos": ["GAME DEMOS", "GAME DEMOS"],
    MENU + "menu_load_campaign": ["CAMPAIGN", "CAMPAIGN"],
    MENU + "menu_multiplayer": ["MULTIPLAYER", "MULTIPLAYER"],
    MENU + "menu_settings": ["SETTINGS", "SETTINGS"],
    MENU + "multiplayer_type_select\\connected\\4way_profile_select\\header_select_profile": ["SELECT PROFILE"],
    MENU + "multiplayer_type_select\\connected\\pregame\\header_enlisted_players": ["SELECT TEAMS", "ENLISTED PLAYERS"],
    MENU + "multiplayer_type_select\\connected\\server_list\\header_found_games": ["SYSTEM LINK GAMES"],
    MENU + "multiplayer_type_select\\coop\\header_select_profile_player_1": ["SELECT PROFILE: PLAYER 1"],
    MENU + "multiplayer_type_select\\coop\\header_select_profile_player_2": ["SELECT PROFILE: PLAYER 2"],
    MENU + "multiplayer_type_select\\header_multiplayer": ["MULTIPLAYER"],
    MENU + "multiplayer_type_select\\mp_map_select\\header_select_map": ["SELECT MAP"],
    MENU + "player_profiles_select\\header_select_profile": ["SELECT PROFILE"],
    MENU + "settings_select\\multiplayer_setup\\indicator_options_edit\\header_indicator_options": ["INDICATOR OPTIONS"],
    MENU + "settings_select\\multiplayer_setup\\item_options_edit\\header_item_options": ["ITEM OPTIONS"],
    MENU + "settings_select\\multiplayer_setup\\name_edit\\header_gametype_name": ["GAMETYPE NAME"],
    MENU + "settings_select\\multiplayer_setup\\player_options_edit\\header_player_options": ["PLAYER OPTIONS"],
    MENU + "settings_select\\multiplayer_setup\\playlist_edit\\game_type_select\\header_rules": ["STEP 1: SELECT GAME"],
    MENU + "settings_select\\multiplayer_setup\\playlist_edit\\header_edit_multiplayer_gametype": ["EDIT MULTIPLAYER GAMETYPE"],
    MENU + "settings_select\\multiplayer_setup\\playlist_edit\\header_set_options": ["STEP 2: SET GAME RULES"],
    MENU + "settings_select\\multiplayer_setup\\playlist_select\\header_select_gametype_to_edit": ["SELECT GAMETYPE TO EDIT"],
    MENU + "settings_select\\player_setup\\header_select_profile_to_edit": ["SELECT PROFILE TO EDIT"],
    MENU + "settings_select\\player_setup\\player_profile_edit\\advanced_controls\\header_advanced_controls": ["ADVANCED CONTROLS"],
    MENU + "settings_select\\player_setup\\player_profile_edit\\color_edit\\header_profile_color": ["MULTIPLAYER COLOR"],
    MENU + "settings_select\\player_setup\\player_profile_edit\\controller_edit\\header_profile_controller_setting": ["CONTROLLER SETUP"],
    MENU + "settings_select\\player_setup\\player_profile_edit\\header_edit_profile_settings": ["EDIT PROFILE SETTINGS"],
    MENU + "settings_select\\player_setup\\player_profile_edit\\name_edit\\header_profile_name": ["PROFILE NAME"],
    MENU + "solo_level_select\\header_load_level": ["LOAD LEVEL"],
    # (in every map: the multiplayer maps show it after a game)
    "ui\\shell\\bitmaps\\postgame_carnage_report": ["POSTGAME CARNAGE REPORT"],
}

# where the text is, in the pictures that hold more than their title (texels:
# left, top, right, bottom, the right and bottom past it): only that part's
# text is set again, the rest of the picture enlarged as a plate is
TEXT_BOXES = {
    # (the report's panel, its border line at row 68 under the title)
    "ui\\shell\\bitmaps\\postgame_carnage_report": (58, 36, 556, 67),
}

# the pictures whose rest is drawn from a hand-made SVG redraw of it (in
# port/assets/titles/svg, without its text) rather than enlarged from the map's
# picture: sharper, and the project's own drawing
BACKGROUNDS = {
    "ui\\shell\\bitmaps\\postgame_carnage_report": "postgame_carnage_report.svg",
}

# a text the game draws in a title's bitmap in place of the map's, by bitmap:
# the text, and the CRC-32 (zlib's) of the pixels the game draws, which its
# picture stands for instead of the map's. QUIT in GAME DEMOS's frames, in the
# builds of a desktop application (source/interface/ui_widget.c, whose
# ui_exit_game_label_checksums are these inverted)
VARIANTS = {
    MENU + "menu_game_demos": [("QUIT", 0x6713993C), ("QUIT", 0xDE2B7763)],
}


def text_mask(image: np.ndarray) -> np.ndarray:
    """The old text's texels. A selected menu item's text is white in a blue
    glow; a header's is bright blue over a dark plate; an unselected item is
    its text alone, flat and translucent."""
    alpha = image[..., 3].astype(float)
    value = image[..., :3].max(axis=-1).astype(float)
    white = (image[..., :3].min(axis=-1) > 200) & (alpha > 128)
    if white.sum() > 40:
        return white
    if ((alpha > 0) & (value < 64)).sum() > 40:
        return (alpha > 0) & (value >= 96)
    return alpha >= 0.5 * alpha.max()


def fit_glow(image: np.ndarray, mask: np.ndarray) -> tuple:
    """The glow around white text as a blur of its letters: the sigma (in the
    map's texels) and gain whose blur of the mask best gives the alpha
    around it."""
    from scipy import ndimage

    alpha = image[..., 3].astype(float) / 255
    outside = ~mask
    best = None
    for sigma in np.arange(0.5, 4.01, 0.25):
        blur = ndimage.gaussian_filter(mask.astype(float), sigma)
        x, y = blur[outside], alpha[outside]
        gain = float((x * y).sum() / max((x * x).sum(), 1e-9))
        error = float(((np.minimum(1, gain * x) - y) ** 2).sum())
        if best is None or error < best[0]:
            best = (error, sigma, gain)
    return best[1], best[2]


def fill_in(image: np.ndarray, mask: np.ndarray) -> np.ndarray:
    """image with the masked texels taken from the nearest unmasked ones (then
    softened), so that the old text leaves no shape behind."""
    from scipy import ndimage

    if not (~mask).any():
        return np.zeros_like(image)
    _, (rows, columns) = ndimage.distance_transform_edt(mask, return_indices=True)
    filled = image[rows, columns].astype(float)
    soft = np.stack([ndimage.uniform_filter(filled[..., c], size=3) for c in range(4)], axis=-1)
    result = image.astype(float).copy()
    result[mask] = soft[mask]
    return result


def enlarge(image: np.ndarray, scale: int, smoothing: float) -> np.ndarray:
    """image (straight RGBA, float) scale times larger, filtered with its
    colour premultiplied, and blurred by smoothing texels first (a plate's
    gradients are steps of 1/16 in a DXT3 texture's alpha)."""
    from scipy import ndimage

    alpha = image[..., 3:4] / 255
    premultiplied = np.concatenate([image[..., :3] * alpha, image[..., 3:4]], axis=-1)
    if smoothing > 0:
        premultiplied = np.stack([ndimage.gaussian_filter(premultiplied[..., c], smoothing, mode="nearest")
                                  for c in range(4)], axis=-1)
    channels = [np.asarray(Image.fromarray(premultiplied[..., c].astype(np.float32), "F").resize(
        (image.shape[1] * scale, image.shape[0] * scale), Image.BICUBIC)) for c in range(4)]
    big = np.clip(np.stack(channels, axis=-1), 0, 255)
    big_alpha = big[..., 3:4] / 255
    rgb = np.where(big_alpha > 1e-4, big[..., :3] / np.maximum(big_alpha, 1e-4), 0)
    return np.concatenate([np.clip(rgb, 0, 255), big[..., 3:4]], axis=-1)


def text_coverage(image: np.ndarray) -> np.ndarray:
    """How much of each texel the old text covers, 0 to 1, read by its style
    (text_mask): a selected item's whiteness, a header's brightness over its
    plate, an unselected item's alpha."""
    from scipy import ndimage

    mask = text_mask(image)
    alpha = image[..., 3].astype(float) / 255
    rgb = image[..., :3].astype(float)
    eroded = ndimage.binary_erosion(mask)
    solid = eroded if eroded.sum() > 20 else mask
    colour = np.median(rgb[solid], axis=0)
    if colour.min() > 200:
        glow = np.median(rgb[~ndimage.binary_dilation(mask) & (alpha > 0.12)].min(axis=-1))
        return np.clip((rgb.min(axis=-1) - glow) / (255 - glow), 0, 1) * (alpha > 0)
    if ((alpha > 0) & (rgb.max(axis=-1) < 64)).sum() > 40:
        green = rgb[..., 1]
        text, plate = np.median(green[solid]), np.median(green[(alpha > 0) & (rgb.max(axis=-1) < 64)])
        return np.clip((green - plate) / (text - plate), 0, 1) * (alpha > 0)
    return np.clip(alpha / np.median(alpha[solid]), 0, 1)


def draw_glyph(font, character: str) -> tuple:
    """A character's ink (0 to 1), and its left and top from the pen on the
    baseline (the font's box has its spacing too: the ink is cut from it); a
    letter in FEET as the other's shape with its foot."""
    shape = FEET.get(character, character)
    left, top, right, bottom = font.getbbox(shape, anchor="ls")
    image = Image.new("L", (max(1, right - left), max(1, bottom - top)), 0)
    ImageDraw.Draw(image).text((-left, -top), shape, font=font, fill=255, anchor="ls")
    box = image.getbbox()
    if box:
        image = image.crop(box)
        left, top = left + box[0], top + box[1]
    ink = np.asarray(image, np.float32) / 255
    if character in FEET:
        _, cap_top, _, cap_bottom = font.getbbox("H", anchor="ls")
        solid = ink >= 0.5
        rows = np.nonzero(solid.any(axis=1))[0]
        widest = np.nonzero(solid.any(axis=0))[0].max()
        for row in range(max(rows.min(), rows.max() + 1 - round(FOOT * (cap_bottom - cap_top))), rows.max() + 1):
            if solid[row].any():
                ink[row, np.nonzero(solid[row])[0].max():widest + 1] = 1
    return ink, left, top


class Layout:
    """The text's letters placed where the old ones are. Each glyph is first
    put where the font's spacing (with kerning) puts it, the whole spread to
    the old text's width; then each is moved, in eighths of a texel, to where
    it covers the old letters best, and the capitals' height and the
    baseline are fitted the same way. Positions are in eighths of the map's
    texels (SUPER)."""

    def __init__(self, text: str, coverage: np.ndarray, box: tuple):
        self.text = text
        self.coverage = coverage
        left, top, right, bottom = box
        # the region compared: the old text's rows and columns, and a margin
        self.rows = (max(0, top - 3), min(coverage.shape[0], bottom + 4))
        self.columns = (max(0, left - 3), min(coverage.shape[1], right + 4))
        self.cap = (bottom - top + 1) * SUPER
        # whether a space comes before each letter
        self.spaced = [index > 0 and text[index - 1] == " " for index, character in enumerate(text) if character != " "]
        self.baseline = (bottom + 1) * SUPER
        self.load()
        # the font's spacing, spread to the old text's width
        pens = self.spacing()
        natural_left, natural_right = self.extent(pens)
        gaps = max(1, len(pens) - 1)
        track = ((right + 1 - left) * SUPER - (natural_right - natural_left)) / gaps
        self.base = [left * SUPER - natural_left + pen for pen in pens]
        self.spread(0.0, track)
        self.natural = list(self.pens)

    def spacing(self) -> list:
        """Each letter's pen at the font's spacing (with its kerning), from
        the first's (a letter in FEET spaced as the shape it is set as)."""
        text = "".join(FEET.get(character, character) for character in self.text)
        pens, pen = [], 0.0
        for index, character in enumerate(text):
            if character != " ":
                pens.append(pen)
            pen += self.font.getlength(character)
            if index + 1 < len(text):
                pair = text[index:index + 2]
                pen += self.font.getlength(pair) - self.font.getlength(pair[0]) - self.font.getlength(pair[1])
        return pens

    def extent(self, pens: list) -> tuple:
        """The ink's left and right with the glyphs at pens."""
        return (pens[0] + self.glyphs[0][1],
                pens[-1] + self.glyphs[-1][1] + self.glyphs[-1][0].shape[1])

    def in_place(self, text: str) -> "Layout":
        """text set in this text's place (a text the game draws in the bitmap
        instead of the map's: QUIT): at its capitals' height and baseline,
        with the font's spacing alone, centred on its ink."""
        old_left, old_right = self.extent(self.pens)
        result = copy.copy(self)
        result.text = text
        result.spaced = [index > 0 and text[index - 1] == " " for index, character in enumerate(text) if character != " "]
        result.load()
        pens = result.spacing()
        natural_left, natural_right = result.extent(pens)
        result.base = [(old_left + old_right - natural_left - natural_right) / 2 + pen for pen in pens]
        result.spread(0.0, 0.0)
        result.natural = list(result.pens)
        return result

    def spread(self, offset: float, track: float):
        """The glyphs at the font's spacing, moved offset and tracking track
        (in eighths of a texel) further apart."""
        self.offset, self.track = offset, track
        self.pens = [round(pen + offset + index * track) for index, pen in enumerate(self.base)]

    def load(self):
        probe = ImageFont.truetype(str(FONT), 1000)
        _, top, _, bottom = probe.getbbox("H")
        self.font = ImageFont.truetype(str(FONT), max(8, int(round(self.cap * 1000 / (bottom - top)))))
        self.glyphs = [self.glyph(character) for character in self.text if character != " "]

    def glyph(self, character):
        """The glyph's ink, and its left and top from the pen on the baseline."""
        return draw_glyph(self.font, character)

    def compose(self, first: int, last: int, skip: int = -1, moved: tuple = None, only=None) -> np.ndarray:
        """The text's coverage over the map's columns first to last (and the
        compared rows), with glyph skip at moved's pen instead (and only the
        glyphs only, if given)."""
        top, bottom = self.rows
        canvas = np.zeros(((bottom - top) * SUPER, (last - first) * SUPER), np.float32)
        for index, ((ink, left, glyph_top), pen) in enumerate(zip(self.glyphs, self.pens)):
            if only is not None and index not in only:
                continue
            if index == skip:
                pen = moved
            x0 = pen + left - first * SUPER
            y0 = self.baseline + glyph_top - top * SUPER
            x1, y1 = x0 + ink.shape[1], y0 + ink.shape[0]
            cx0, cy0, cx1, cy1 = max(0, x0), max(0, y0), min(canvas.shape[1], x1), min(canvas.shape[0], y1)
            if cx1 > cx0 and cy1 > cy0:
                np.maximum(canvas[cy0:cy1, cx0:cx1], ink[cy0 - y0:cy1 - y0, cx0 - x0:cx1 - x0], out=canvas[cy0:cy1, cx0:cx1])
        return canvas.reshape(bottom - top, SUPER, last - first, SUPER).mean(axis=(1, 3))

    def sums(self, first: int, last: int, letters: np.ndarray) -> tuple:
        old = self.coverage[self.rows[0]:self.rows[1], first:last]
        return float(np.minimum(old, letters).sum()), float(np.maximum(old, letters).sum())

    def overlap(self) -> float:
        both, either = self.sums(*self.columns, self.compose(*self.columns))
        return both / max(either, 1e-9)

    def fit(self):
        first, last = self.columns
        # the title's own spacing first: the whole text moved and tracked,
        # searched over a grid, then refined
        offset0, track0 = self.offset, self.track
        scored = []
        for offset in np.arange(-4, 4.01, 0.5) * SUPER:
            for track in np.arange(-8, 8.01, 1.0):
                self.spread(offset0 + offset, track0 + track)
                scored.append((self.overlap(), (offset0 + offset, track0 + track)))
        self.spread(*max(scored)[1])
        for step in (2, 1, 0.5, 0.25):
            while True:
                current = self.overlap()
                offset, track = self.offset, self.track
                trials = [(offset + d * step * 2, track) for d in (-1, 1)] + [(offset, track + d * step) for d in (-1, 1)]
                scored = []
                for candidate in trials:
                    self.spread(*candidate)
                    scored.append((self.overlap(), candidate))
                best = max(scored)
                if best[0] > current + 1e-6:
                    self.spread(*best[1])
                else:
                    self.spread(offset, track)
                    break
        self.natural = list(self.pens)
        for _ in range(3):
            for index in range(len(self.glyphs)):
                ink, left, _ = self.glyphs[index]
                for step in (8, 4, 2, 1):
                    while True:
                        pen = self.pens[index]
                        a = max(first, (pen + left - 2 * SUPER) // SUPER - 2)
                        b = min(last, (pen + left + ink.shape[1] + 2 * SUPER) // SUPER + 3)
                        if b <= a:
                            break
                        both, either = self.sums(a, b, self.compose(a, b))
                        best = (both / max(either, 1e-9), pen)
                        for candidate in (pen - step, pen + step):
                            if abs(candidate - self.natural[index]) > MAXIMUM_MOVE or not self.in_order(index, candidate):
                                continue
                            both, either = self.sums(a, b, self.compose(a, b, index, candidate))
                            if both / max(either, 1e-9) > best[0] + 1e-6:
                                best = (both / max(either, 1e-9), candidate)
                        if best[1] == pen:
                            break
                        self.pens[index] = best[1]
            # the capitals' height and the baseline
            for what in ("cap", "baseline"):
                for step in (4, 2, 1):
                    while True:
                        current = self.overlap()
                        improved = False
                        for delta in (-step, step):
                            setattr(self, what, getattr(self, what) + delta)
                            if what == "cap":
                                self.load()
                            if self.overlap() > current + 1e-6:
                                improved = True
                                break
                            setattr(self, what, getattr(self, what) - delta)
                            if what == "cap":
                                self.load()
                        if not improved:
                            break
        return self

    @staticmethod
    def gap(row: np.ndarray, right: int, left: int):
        """The gap between two letters in a row of coverage, as an area: the
        uncovered part of every texel from the left letter's last solid one
        to the right letter's first (looked for around right, the left
        letter's last texel, and left, the right one's first); None if either
        has no solid texel there. A texel the two letters share counts once,
        so the game's and ours are measured alike."""
        solid = row >= SOLID
        start = None
        for x in range(min(len(row) - 1, right + 2), max(-1, right - GAP_SEARCH), -1):
            if solid[x]:
                start = x
                break
        stop = None
        for x in range(max(0, left - 2), min(len(row), left + GAP_SEARCH)):
            if solid[x] and (start is None or x > start):
                stop = x
                break
        if start is None or stop is None or stop <= start:
            return None
        return float((1 - row[start:stop + 1]).sum())

    def gaps(self, letters: np.ndarray) -> list:
        """For each two letters not parted by a space, the narrowest gap
        between their ink in the old text and in letters (both the text's
        coverage over the compared columns and rows, in texels), measured row
        by row the same way in both (gap), in the rows where both letters
        have solid texels (None if there are none)."""
        first, last = self.columns
        old = self.coverage[self.rows[0]:self.rows[1], first:last]
        result = []
        for index in range(len(self.glyphs) - 1):
            if self.spaced[index + 1]:
                result.append(None)
                continue
            before = self.compose(first, last, only={index})
            after = self.compose(first, last, only={index + 1})
            pairs = []
            for row in range(old.shape[0]):
                right = np.nonzero(before[row] >= SOLID)[0]
                left = np.nonzero(after[row] >= SOLID)[0]
                if not len(right) or not len(left):
                    continue
                measured = [self.gap(coverage, right.max(), left.min()) for coverage in (old[row], letters[row])]
                if None not in measured:
                    pairs.append(measured)
            result.append((min(p[0] for p in pairs), min(p[1] for p in pairs)) if pairs else None)
        return result

    def match_gaps(self):
        """Letters nearly touching (GAP_NEAR) where the old ones are further
        apart parted, so that the narrowest gap between them is the old
        text's (its letters' padding),
        the letters otherwise as little moved as they can be from where they
        cover the old letters best: by least squares, the gaps weighed
        GAP_WEIGHT times the positions."""
        anchor = list(self.pens)
        for _ in range(3):
            letters = self.compose(*self.columns)
            gaps = self.gaps(letters)
            count = len(self.pens)
            rows, values = [], []
            for index, pen in enumerate(anchor):
                row = np.zeros(count)
                row[index] = 1
                rows.append(row)
                values.append(pen)
            for index, gap in enumerate(gaps):
                if gap is None:
                    continue
                old, ours = gap
                # (only letters nearly touching are parted: elsewhere, covering
                # the old letters decides, the gaps read less surely)
                if ours >= old - GAP_TOLERANCE or ours >= GAP_NEAR:
                    continue
                shift = float(min(old - ours, 1.5)) * SUPER
                row = np.zeros(count)
                row[index + 1], row[index] = GAP_WEIGHT, -GAP_WEIGHT
                rows.append(row)
                values.append(GAP_WEIGHT * (self.pens[index + 1] - self.pens[index] + shift))
            solution, *_ = np.linalg.lstsq(np.array(rows), np.array(values), rcond=None)
            self.pens = [int(round(value)) for value in solution]
        return self

    def render(self, scale: int, size: tuple) -> np.ndarray:
        """The text's coverage on a canvas of size, scale times the map's
        texels, its glyphs drawn at that size where they were placed."""
        factor = scale * 2
        probe = ImageFont.truetype(str(FONT), 1000)
        _, top, _, bottom = probe.getbbox("H")
        font = ImageFont.truetype(str(FONT), max(8, int(round(self.cap / SUPER * factor * 1000 / (bottom - top)))))
        canvas = np.zeros((size[0] * 2, size[1] * 2), np.float32)
        baseline = round(self.baseline / SUPER * factor)
        characters = [character for character in self.text if character != " "]
        for character, pen in zip(characters, self.pens):
            ink, left, glyph_top = draw_glyph(font, character)
            x0, y0 = round(pen / SUPER * factor) + left, baseline + glyph_top
            x1, y1 = x0 + ink.shape[1], y0 + ink.shape[0]
            cx0, cy0, cx1, cy1 = max(0, x0), max(0, y0), min(canvas.shape[1], x1), min(canvas.shape[0], y1)
            np.maximum(canvas[cy0:cy1, cx0:cx1], ink[cy0 - y0:cy1 - y0, cx0 - x0:cx1 - x0], out=canvas[cy0:cy1, cx0:cx1])
        return canvas.reshape(size[0], 2, size[1], 2).mean(axis=(1, 3))

    def in_order(self, index: int, pen: int) -> bool:
        """Whether glyph index at pen keeps its ink's left after the middle of
        the glyph before it, and its middle before the next one's left."""
        ink, left, _ = self.glyphs[index]
        start = pen + left
        if index > 0:
            before, before_left, _ = self.glyphs[index - 1]
            if start < self.pens[index - 1] + before_left + before.shape[1] / 2:
                return False
        if index + 1 < len(self.glyphs):
            _, after_left, _ = self.glyphs[index + 1]
            if start + ink.shape[1] / 2 > self.pens[index + 1] + after_left:
                return False
        return True

    def lefts(self) -> list:
        """Each letter's ink's left edge, in texels."""
        return [round((pen + left) / SUPER, 3) for pen, (_, left, _) in zip(self.pens, self.glyphs)]

    def corrections(self) -> list:
        """How far each letter was moved from the font's spacing (at the
        title's own tracking), in texels."""
        return [round((pen - natural) / SUPER, 3) for pen, natural in zip(self.pens, self.natural)]


def bleed(image: np.ndarray) -> np.ndarray:
    """Transparent texels given their nearest covered texel's colour, so that
    filtering at the edges blends in no black."""
    from scipy import ndimage

    covered = image[..., 3] > 0
    if not covered.any() or covered.all():
        return image
    _, (rows, columns) = ndimage.distance_transform_edt(~covered, return_indices=True)
    result = image.copy()
    result[..., :3] = np.where(covered[..., None], image[..., :3], image[rows, columns, :3])
    return result


def build_title(bitmap: dict, text: str, scale: int, layout=None, region=None, svg=None, margin: int = 1) -> tuple:
    """The title's picture, and its letters' layout (layout: one to use, the
    same text's in another picture of the same group; region: where the text
    is, if not all over the picture, TEXT_BOXES; svg: the picture's redraw to
    set the text over, BACKGROUNDS; margin: how far around the old text, in
    texels, is taken out with it)."""
    from scipy import ndimage

    image = decode_bitmap(bitmap)
    if region:
        left, top, right, bottom = region
        mask = np.zeros(image.shape[:2], bool)
        coverage_old = np.zeros(image.shape[:2], float)
        mask[top:bottom, left:right] = text_mask(image[top:bottom, left:right])
        coverage_old[top:bottom, left:right] = text_coverage(image[top:bottom, left:right])
    else:
        mask = text_mask(image)
        coverage_old = text_coverage(image)
    ys, xs = np.where(coverage_old >= 0.5)
    box = (xs.min(), ys.min(), xs.max(), ys.max())
    # the text's colour and alpha: those of its solid texels
    eroded = ndimage.binary_erosion(mask)
    solid = eroded if eroded.sum() > 20 else mask
    colour = np.median(image[solid][:, :3].astype(float), axis=0)
    opacity = np.median(image[solid][:, 3].astype(float)) / 255
    if layout is None:
        layout = Layout(text, coverage_old, box).fit().match_gaps()
    letters = layout.render(scale, (image.shape[0] * scale, image.shape[1] * scale))
    edge = ndimage.binary_dilation(mask, iterations=margin)
    if svg:
        # the redraw, drawn at the title's size
        background = render_svg(TITLES / "svg" / svg, scale).astype(float)
        if background.shape[:2] != (image.shape[0] * scale, image.shape[1] * scale):
            raise SystemExit(f"{svg}: not {scale}x its bitmap's {image.shape[1]}x{image.shape[0]}")
    elif colour.min() > 200 and not region:
        # white text: its glow grown again around the new letters
        sigma, gain = fit_glow(image, mask)
        glow_colour = np.median(image[~edge & (image[..., 3] > 32)][:, :3].astype(float), axis=0)
        glow = np.minimum(1, gain * ndimage.gaussian_filter(letters, sigma * scale))
        background = np.concatenate([np.broadcast_to(glow_colour, glow.shape + (3,)), glow[..., None] * 255], axis=-1)
    else:
        # the background (the plate) with the old text and its edge filled in
        background = enlarge(fill_in(image, edge), scale, SMOOTHING)
    coverage = letters * opacity
    # the text over the background
    back_alpha = background[..., 3] / 255
    out_alpha = coverage + back_alpha * (1 - coverage)
    out_rgb = colour * coverage[..., None] + background[..., :3] * (back_alpha * (1 - coverage))[..., None]
    out_rgb = np.where(out_alpha[..., None] > 1e-4, out_rgb / np.maximum(out_alpha[..., None], 1e-4), 0)
    result = np.concatenate([out_rgb, out_alpha[..., None] * 255], axis=-1)
    return bleed(np.clip(np.round(result), 0, 255).astype(np.uint8)), layout


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--map", required=True)
    arguments = parser.parse_args()
    xbox_map = XboxMap(Path(arguments.map))
    TITLES.mkdir(parents=True, exist_ok=True)
    for stale in TITLES.glob("*.png"):
        stale.unlink()
    entries = []
    layouts = {}
    for tag, texts in TEXTS.items():
        group = xbox_map.bitmap_group(tag)
        for index, (bitmap, text) in enumerate(zip(group["bitmaps"], texts)):
            width, height = bitmap["width"], bitmap["height"]
            scale = SCALE
            while max(width, height) * scale > MAXIMUM_SIZE:
                scale //= 2
            stem = tag.split("\\")[-2] + "__" + tag.split("\\")[-1]
            name = f"{stem}__{index}"
            # (a selected menu item's text is its unselected picture's, which
            # is read more surely: flat, where the selected one's white blurs
            # into its glow)
            image, layout = build_title(bitmap, text, scale, layouts.get((tag, text)), TEXT_BOXES.get(tag),
                                        BACKGROUNDS.get(tag))
            layouts.setdefault((tag, text), layout)
            Image.fromarray(image, "RGBA").save(TITLES / f"{name}.png", optimize=True)
            entries.append({
                "name": name,
                "tag": tag,
                "bitmap": index,
                "width": width,
                "height": height,
                "format": FORMATS[bitmap["format"]],
                "scale": scale,
                "crc": zlib.crc32(bitmap["pixels"][:level0_size(bitmap)]),
                "text": text,
                # where each letter's ink starts, and the capitals' height (texels):
                # tools/title_font.py --measure reads them
                "lefts": layout.lefts(),
                "cap": layout.cap / SUPER,
            })
            moved = np.abs(layout.corrections())
            print(f"{name}: '{text}' {width}x{height} at {scale}x, covers {layout.overlap():.3f}; "
                  f"letters moved {moved.mean():.2f} texels on average, {moved.max():.2f} at most")
            if tag in VARIANTS:
                # (the game's own text there, standing for its own pixels)
                variant, crc = VARIANTS[tag][index]
                variant_name = f"{stem}__{variant.lower().replace(' ', '_')}__{index}"
                image, placed = build_title(bitmap, variant, scale, layout.in_place(variant), margin=HALO)
                Image.fromarray(image, "RGBA").save(TITLES / f"{variant_name}.png", optimize=True)
                entries.append({**entries[-1], "name": variant_name, "crc": crc, "text": variant,
                                "lefts": placed.lefts(), "cap": placed.cap / SUPER, "in_place_of": text})
                print(f"{variant_name}: '{variant}' set in '{text}''s place")
    (TITLES / "titles.json").write_text(json.dumps({"font": FONT.name, "assets": entries}, indent=1) + "\n")


if __name__ == "__main__":
    sys.exit(main())
