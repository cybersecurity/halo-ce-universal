#!/usr/bin/env python3
"""Writes the PC version's menus (Halo Custom Edition's) as the port's menu
files (port/assets/menus/ce, read by port/linux/src/menu_files.c):

    python tools/ce_menus.py --tags .../halopc-restored/tags \\
        --definitions .../invader/src/tag/hek/definition \\
        --redraws .../ui-svg-handmade --placeholder .../placeholder.png \\
        [--pictures .../extracted-ui-assets-fixed]

Every widget reachable from the PC main menu (ui\\shell\\main_menu\\main_menu:
its children, the widgets its handlers open, their conditional widgets and
list descriptions, and so on) is written as a <widget>, laid out as the PC
version has it, with its string lists as <strings> and its bitmaps as
<bitmap>s. Our widgets are named by their tag's path below ui\\shell, with
forward slashes (main_menu/main_menu); the root is the main menu.

The bitmaps are drawn from the high-res redraws (--redraws, hand-made SVGs
in the PC bitmaps' layout), copied to port/assets/menus/svg, at up to 4
times their size. The pictures with no redraw (renders, screenshots, logos)
are the Xbox map's bitmap of the same name, where it has the same pictures
in the same order (XBOX_BITMAPS: the player's own ui.map draws them, and
nothing is shipped), else the --placeholder image, fitted to each frame
where the PC version's picture is (--pictures: only their bounds are read),
else to the part of it the widgets show;
port/assets/menus/NON_HANDDRAWN.md lists both, the placeholders as ones to
be redrawn.

What this engine (the Xbox's) has no use for is written but never runs: the
PC version's mouse events (the port's own mouse support clicks with A), and
its own functions (port/linux/game/menu_functions.c, most of which do
nothing yet). Its gamespy and ticker fonts are drawn with small_ui, which
the Xbox's map has.

Needs rsvg-convert, Pillow, NumPy and SciPy; port/assets/menus/menus.json,
which tools/embed_assets.py embeds the files from, is written too.
"""

import argparse
import json
import shutil
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path
from typing import Optional
from xml.sax.saxutils import quoteattr

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from hud_assets import bleed, render_svg  # noqa: E402
import port_settings  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
MENUS = ROOT / "port/assets/menus"
CE = MENUS / "ce"
SVG_COPIES = MENUS / "svg"
MAIN_MENU = "ui\\shell\\main_menu\\main_menu"
TITLE_FONT = ROOT / "port/assets/fonts/OpenCE-Regular.ttf"
# the widgets left out, with their pictures (and all they alone lead to):
# the server browser's GameSpy logo, which the port has no use for, and the
# main menu's Credits, which the Xbox's game has not
LEFT_OUT = {
    "ui\\shell\\main_menu\\multiplayer_type_select\\join_game\\header_gamespy_logo",
    "ui\\shell\\main_menu\\main_menu_item_credits",
    # (the "checking for updates" dialogs, which asked the PC version's
    # servers: Create > Internet and Join > Server Browser skip them,
    # port_settings.WIDGET_PATCHES)
    "ui\\shell\\main_menu\\multiplayer_type_select\\checking_updates_screen_create",
    "ui\\shell\\main_menu\\multiplayer_type_select\\checking_updates_screen_join",
}
# children moved: (parent, child) to (x, y); the main menu's Quit up into
# Credits' place
CHILD_OFFSETS = {
    ("ui\\shell\\main_menu\\main_menu_select_list", "ui\\shell\\main_menu\\main_menu_item_quit_game"): (192, 391),
    # the browser's rows up under the column titles, into the place of the
    # scroll up button (hidden: the list does not scroll)
    **{("ui\\shell\\main_menu\\multiplayer_type_select\\join_game\\join_game_items_list",
        f"ui\\shell\\main_menu\\multiplayer_type_select\\join_game\\server_item_{row}"): (10, 102 + 17 * (row - 1))
       for row in range(1, 16)},
}
SCALE = 4
MAXIMUM_SIZE = 2048
# the PC fonts this engine's map lacks, and the font drawn instead
FONTS = {"ui\\gamespy": "ui\\small_ui", "ui\\ticker": "ui\\small_ui"}

# the XML's names of the tags' values (menu_tags.c has the same)
EVENTS = ["a", "b", "x", "y", "black", "white", "left_trigger", "right_trigger", "up", "down", "left", "right",
          "start", "back", "left_thumb", "right_thumb", "stick_up", "stick_down", "stick_left", "stick_right",
          "right_stick_up", "right_stick_down", "right_stick_left", "right_stick_right", "created", "deleted",
          "get_focus", "lose_focus", "left_mouse", "middle_mouse", "right_mouse", "double_click",
          "custom_activation", "post_render"]
WIDGET_FLAGS = ["pass_unhandled_to_focused_child", "pause_game", "flash_bitmap", "up_down_tabs_children",
                "left_right_tabs_children", "up_down_tabs_items", "left_right_tabs_items", "no_focused_child",
                "pass_unhandled_to_all_children", "render_any_controller", "pass_handled_to_all_children",
                "main_menu_if_no_history", "tag_controller_index", "nifty_fx", "no_history", "force_handle_mouse",
                "no_widescreen_fill"]
TEXT_FLAGS = ["editable", "password", "flashing", "no_focus_test"]
LIST_FLAGS = ["items_in_code", "items_from_strings", "one_tooltip", "single_preview"]
TYPES = ["container", "text", "spinner", "column_list"]
ALIGN = ["left", "right", "center"]
REPLACE = [None, "controller", "build_number", "pid"]

# the PC bitmaps with no redraw whose pictures the Xbox's ui.map has, under
# the same name and in the same order: drawn from the player's map. (The
# Xbox's mp_map_grafix has the 13 Xbox maps' pictures, as the PC version's
# first 13, and its "?" after them; the PC version's other 8 are its own
# maps', which the Xbox's game has not.)
XBOX_BITMAPS = {
    "ui\\shell\\bitmaps\\colors_sm", "ui\\shell\\bitmaps\\mp_map_grafix", "ui\\shell\\bitmaps\\sp_levels",
    "ui\\shell\\main_menu\\difficulty_select\\difficulty_options",
    "ui\\shell\\main_menu\\difficulty_select\\difficulty_options_small", "ui\\shell\\main_menu\\halo_logo",
    "ui\\shell\\main_menu\\settings_select\\player_setup\\player_profile_edit\\color_edit\\player_color_marine_large",
}
# the frames with no redraw that are drawn from a frame of the Xbox map's
# bitmap of the same name, whose other frames differ (<frame map=...>):
# the same picture, or for the profile settings the Xbox's for the same
# thing (Change Name; its Controller Setup for Gamepads)
XBOX_FRAMES = {
    "shell/main_menu/multiplayer_type_select/mp_options__2": 3,
    "shell/main_menu/settings_select/multiplayer_setup/playlist_edit/gametype_options__2": 2,
    "shell/main_menu/settings_select/multiplayer_setup/playlist_edit/gametype_options__5": 4,
    "shell/main_menu/settings_select/player_setup/player_profile_edit/profile_options__0": 0,
    "shell/main_menu/settings_select/player_setup/player_profile_edit/profile_options__2": 1,
    "shell/main_menu/settings_select/player_setup/player_profile_edit/profile_options__7": 3,
}
PLACEHOLDER_SCALE = 2

# the Xbox's functions that the PC version rewrote for its own widgets (its
# column lists where the Xbox had spinners, and other children), which stop
# this engine on the PC widgets: written as "unwired <name>", which does
# nothing (UNWIRED.md)
INCOMPATIBLE = set()
# the PC version's own functions that menu_functions.c does what they do
# (and its campaign's: the Xbox's of those names, which take the Xbox's
# widgets, menu_functions.c's for ours)
WIRED = {
    "main menu quit game", "profile set edit begin", "mouse emit accept event", "mouse emit back event",
    "mouse emit x event", "emit custom activation event", "single prev cl item activated", "controls back handler",
    "gamespy back handler", "gamespy dismiss error", "gamespy dismiss filters", "gamespy screen init", "mp type set mode",
    "campaign menu init", "campaign menu continue", "difficulty item select", "solo map list update",
    "load game menu init", "load game menu dispose", "load game menu activated", "load game list update",
    "load game menu delete request", "load game menu delete finish",
    "controls screen init", "controls begin binding", "controls screen change set", "controls screen defaults",
    "controls update menu", "profile manager select", "direct ip connect go",
}

# ---------- HEK tags


class Tags:
    """HEK tags (big-endian), read with invader's definitions"""

    BASIC = {"int8": 1, "uint8": 1, "int16": 2, "uint16": 2, "int32": 4, "uint32": 4, "float": 4, "Fraction": 4,
             "Angle": 4, "TagFourCC": 4, "TagID": 4, "Pointer": 4, "Index": 2, "Point2D": 8, "Point2DInt": 4,
             "Rectangle2D": 8, "ColorARGB": 16, "ColorRGB": 12, "TagString": 32, "TagDependency": 16,
             "TagReflexive": 12, "TagDataOffset": 20}
    STRUCTS = {"ui_widget_definition": "UIWidgetDefinition", "bitmap": "Bitmap",
               "unicode_string_list": "UnicodeStringList"}

    def __init__(self, tags: Path, definitions: Path):
        self.root = tags
        self.types = {}
        for path in sorted(definitions.glob("*.json")):
            for item in json.loads(path.read_text()):
                self.types.setdefault(item["name"], item)

    def size(self, field: dict) -> int:
        kind = field["type"]
        if kind == "pad":
            return field["size"]
        if kind in self.BASIC:
            return self.BASIC[kind] * field.get("count", 1)
        definition = self.types[kind]
        if definition["type"] == "enum":
            return 2
        if definition["type"] == "bitfield":
            return definition["width"] // 8
        return definition["size"] * field.get("count", 1)

    def options(self, name: str) -> list:
        return [o if isinstance(o, str) else o["name"] for o in self.types[name]["options"]]

    def parse(self, name: str, data: bytes, base: int, cursor: list) -> dict:
        """the struct at base; what it points to follows from cursor[0], in
        the order of its fields"""
        result, position, later = {}, base, []
        for field in self.types[name]["fields"]:
            kind, key, size = field["type"], field.get("name"), self.size(field)
            raw = data[position:position + size]
            position += size
            if kind == "pad" or not key:
                continue
            if kind == "TagString":
                result[key] = raw.split(b"\0")[0].decode("latin-1")
            elif kind == "TagDependency":
                _, _, length, _ = struct.unpack(">4sIiI", raw)
                result[key] = None
                later.append(("dependency", key, length, None))
            elif kind == "TagReflexive":
                count = struct.unpack(">i", raw[:4])[0]
                result[key] = []
                later.append(("block", key, count, field["struct"]))
            elif kind == "TagDataOffset":
                result[key] = b""
                later.append(("data", key, struct.unpack(">i", raw[:4])[0], None))
            elif kind == "Rectangle2D":
                result[key] = struct.unpack(">4h", raw)
            elif kind == "ColorARGB":
                result[key] = struct.unpack(">4f", raw)
            elif kind in ("float", "Fraction", "Angle"):
                result[key] = struct.unpack(">f", raw)[0]
            elif size == 2:
                unsigned = kind == "uint16" or self.types.get(kind, {}).get("type") == "bitfield"
                result[key] = struct.unpack(">H" if unsigned else ">h", raw)[0]
            elif size == 4:
                unsigned = kind == "uint32" or self.types.get(kind, {}).get("type") == "bitfield"
                result[key] = struct.unpack(">I" if unsigned else ">i", raw)[0]
            else:
                result[key] = raw
        for kind, key, count, element in later:
            if kind == "dependency":
                if count > 0:
                    result[key] = data[cursor[0]:cursor[0] + count].decode("latin-1")
                    cursor[0] += count + 1
            elif kind == "data":
                result[key] = data[cursor[0]:cursor[0] + count]
                cursor[0] += count
            else:
                element_size = self.types[element]["size"]
                start = cursor[0]
                cursor[0] += element_size * count
                result[key] = [self.parse(element, data, start + index * element_size, cursor)
                               for index in range(count)]
        return result

    def path(self, tag: str, group: str) -> Path:
        return self.root / (tag.replace("\\", "/") + "." + group)

    def load(self, tag: str, group: str) -> dict:
        data = self.path(tag, group).read_bytes()
        name = self.STRUCTS[group]
        return self.parse(name, data, 64, [64 + self.types[name]["size"]])


# ---------- names


def our_name(tag: str) -> str:
    """a tag's name in our files: its path below ui\\shell, with slashes"""
    for prefix in ("ui\\shell\\", "ui\\"):
        if tag.startswith(prefix):
            return tag[len(prefix):].replace("\\", "/")
    return tag.replace("\\", "/")


def attributes(pairs: list) -> str:
    return "".join(f" {key}={quoteattr(str(value))}" for key, value in pairs if value is not None)


def text_value(utf16: bytes) -> str:
    text = utf16.decode("utf-16-le").split("\0")[0]
    return text.replace("\r\n", "\\n").replace("\n", "\\n").replace("\r", "")


def flag_names(value: int, names: list) -> str:
    return " ".join(name for bit, name in enumerate(names) if value >> bit & 1) or None


def bounds_text(bounds: tuple) -> str:
    return " ".join(str(value) for value in bounds)


# ---------- the art


class Art:
    def __init__(self, tags: Tags, redraws: Path, placeholder: Path, shown: dict, pictures: Optional[Path]):
        self.tags, self.redraws, self.shown, self.pictures_folder = tags, redraws, shown, pictures
        self.placeholder = Image.open(placeholder).convert("RGBA")
        self.pictures = []  # (frame, size, PC picture) of the frames drawn as the placeholder
        self.xbox_frames = []  # (PC frame, Xbox bitmap, its frame) drawn from the Xbox map
        self.svgs = []
        self.pngs = []

    def title(self, name: str, text: str) -> str:
        """a <bitmap> of a title this port has that the PC version has not:
        the text set in OpenCE (the public-domain Newtown respaced, as the
        port's titles are: tools/title_assets.py) over the PC headers'
        backdrop"""
        from PIL import ImageDraw, ImageFont
        scale = SCALE
        probe = ImageFont.truetype(str(TITLE_FONT), 1000)
        left, top, right, bottom = probe.getbbox("H")
        font = ImageFont.truetype(str(TITLE_FONT), round(port_settings.TITLE_CAP * scale * 1000 / (bottom - top)))
        width = font.getlength(text) / scale
        svg = MENUS / "port_svg" / f"{name}.svg"
        svg.parent.mkdir(parents=True, exist_ok=True)
        svg.write_text(port_settings.title_backdrop(width))
        image = Image.fromarray(render_svg(svg, scale), "RGBA")
        draw = ImageDraw.Draw(image)
        cap_top = font.getbbox("H")[1]
        draw.text((port_settings.TITLE_LEFT * scale, port_settings.TITLE_TOP * scale - cap_top), text,
                  font=font, fill=port_settings.TITLE_COLOR)
        png = f"ce/port/{name}.png"
        (MENUS / png).parent.mkdir(parents=True, exist_ok=True)
        image.save(MENUS / png, optimize=True)
        self.pngs.append(png)
        return "\n".join([f"\t<bitmap{attributes([('name', name)])}>",
                          f"\t\t<frame{attributes([('png', png), ('width', 512), ('height', 64)])}/>",
                          "\t</bitmap>"])

    def frame_sources(self, relative: str, index: int, count: int) -> list:
        return [relative, f"{relative}__0"] if count == 1 else [f"{relative}__{index}"]

    def bitmap(self, tag: str) -> str:
        """a <bitmap> for the bitmap tag, its frames drawn"""
        group = self.tags.load(tag, "bitmap")
        data, sequences = group["bitmap data"], group["bitmap group sequence"]
        relative = tag.replace("\\", "/")[3:]
        if sequences and sequences[0]["bitmap count"]:
            first = sequences[0]["first bitmap index"]
            indices = list(range(first, first + sequences[0]["bitmap count"]))
        elif sequences and sequences[0]["sprites"]:
            sys.exit(f"{tag}: sprites are not supported")
        else:
            indices = list(range(len(data)))
        lines = [f"\t<bitmap{attributes([('name', our_name(tag))])}>"]
        for index in indices:
            width, height = data[index]["width"], data[index]["height"]
            name = self.frame_sources(relative, index, len(data))[-1]
            if name in XBOX_FRAMES and not any((self.redraws / f"{source}.svg").is_file()
                                               for source in self.frame_sources(relative, index, len(data))):
                self.xbox_frames.append((name, tag, XBOX_FRAMES[name]))
                lines.append(f"\t\t<frame{attributes([('map', tag), ('index', XBOX_FRAMES[name])])}/>")
                continue
            png = f"ce/{relative}__{index}.png"
            self.draw(relative, index, len(data), width, height, MENUS / png, self.shown.get(tag, []))
            self.pngs.append(png)
            lines.append(f"\t\t<frame{attributes([('png', png), ('width', width), ('height', height)])}/>")
        lines.append("\t</bitmap>")
        return "\n".join(lines)

    def draw(self, relative: str, index: int, count: int, width: int, height: int, output: Path,
             shown: list) -> None:
        output.parent.mkdir(parents=True, exist_ok=True)
        for name in self.frame_sources(relative, index, count):
            svg = self.redraws / f"{name}.svg"
            if svg.is_file():
                copy = SVG_COPIES / f"{name}.svg"
                copy.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(svg, copy)
                self.svgs.append(copy)
                scale = max(1, min(SCALE, MAXIMUM_SIZE // max(width, height)))
                image = render_svg(copy, scale)
                if image.shape[:2] != (height * scale, width * scale):
                    sys.exit(f"{svg}: {image.shape[1]}x{image.shape[0]}, not {scale}x {width}x{height}")
                Image.fromarray(bleed(image), "RGBA").save(output, optimize=True)
                return
        # (no redraw: the placeholder, as large as fits, in the middle of where
        # the PC version's picture is (only its bounds are read), else of the
        # part the widgets show: a widget draws its bitmap from the top left,
        # cut to its size)
        name = self.frame_sources(relative, index, count)[-1]
        image = Image.new("RGBA", (width * PLACEHOLDER_SCALE, height * PLACEHOLDER_SCALE), (0, 0, 0, 0))
        area = (0, 0, min([width] + [w for w, _ in shown]), min([height] + [h for _, h in shown]))
        original = self.pictures_folder / f"{name}.png" if self.pictures_folder else None
        if original and original.is_file():
            with Image.open(original) as picture:
                if picture.size == (width, height):
                    bounds = picture.convert("RGBA").getchannel("A").point(lambda value: 255 if value > 16 else 0).getbbox()
                    if bounds:
                        area = (bounds[0], bounds[1], min(bounds[2], area[2]), min(bounds[3], area[3]))
        area = tuple(value * PLACEHOLDER_SCALE for value in area)
        picture = self.placeholder.copy()
        picture.thumbnail((area[2] - area[0], area[3] - area[1]), Image.LANCZOS)
        image.paste(picture, ((area[0] + area[2] - picture.width) // 2, (area[1] + area[3] - picture.height) // 2))
        image.save(output, optimize=True)
        self.pictures.append((output.relative_to(MENUS).as_posix(), width, height, f"{name}.png"))


# ---------- the widgets


def reachable(tags: Tags) -> dict:
    widgets, queue = {}, [MAIN_MENU]
    while queue:
        tag = queue.pop()
        if tag in widgets:
            continue
        widget = tags.load(tag, "ui_widget_definition")
        widgets[tag] = widget
        for reference in ([child["widget tag"] for child in widget["child widgets"]] +
                          [handler["widget tag"] for handler in widget["event handlers"]] +
                          [conditional["widget tag"] for conditional in widget["conditional widgets"]] +
                          [widget["extended description widget"]]):
            if reference and reference not in LEFT_OUT:
                queue.append(reference)
    return widgets


def bitmap_name(tag: str) -> str:
    """a bitmap: the Xbox map's (its path) for those it has, else ours"""
    return tag if tag in XBOX_BITMAPS else our_name(tag)


def widget_xml(tag: str, widget: dict, tags: Tags, functions: list, inputs: list, fonts: Counter,
               children_of: dict, depth: int = 1) -> list:
    """the <widget>, and its children's where it is their only parent"""
    indent = "\t" * depth
    bounds = widget["bounds"]
    top, left, bottom, right = bounds
    pairs = [
        ("name", our_name(tag)),
        ("type", TYPES[widget["widget type"]] if widget["widget type"] else None),
        ("controller", None if widget["controller index"] == 4 else str(widget["controller index"] + 1)),
        ("left", left or None), ("top", top or None), ("width", right - left), ("height", bottom - top),
        ("flags", flag_names(widget["flags"], WIDGET_FLAGS)),
        ("auto_close", widget["milliseconds to auto close"] or None),
        ("auto_close_fade", widget["milliseconds auto close fade time"] or None),
        ("bitmap", bitmap_name(widget["background bitmap"]) if widget["background bitmap"] else None),
    ]
    if widget["text label unicode strings list"]:
        pairs.append(("string_list", our_name(widget["text label unicode strings list"])))
        pairs.append(("string_index", widget["string list index"] or None))
    if widget["text font"]:
        font = FONTS.get(widget["text font"], widget["text font"])
        fonts[widget["text font"]] += 1
        pairs.append(("font", font))
    alpha, red, green, blue = widget["text color"]
    if (alpha, red, green, blue) != (0, 0, 0, 0):
        pairs.append(("color", "#%02X%02X%02X%02X" % tuple(round(c * 255) for c in (alpha, red, green, blue))))
    pairs += [
        ("align", ALIGN[widget["justification"]] if widget["justification"] else None),
        ("text_flags", flag_names(widget["flags 1"], TEXT_FLAGS)),
        ("text_x", widget["horiz offset"] or None), ("text_y", widget["vert offset"] or None),
        ("list_flags", flag_names(widget["flags 2"], LIST_FLAGS)),
        ("header_bitmap", bitmap_name(widget["list header bitmap"]) if widget["list header bitmap"] else None),
        ("footer_bitmap", bitmap_name(widget["list footer bitmap"]) if widget["list footer bitmap"] else None),
        ("header_bounds", bounds_text(widget["header bounds"]) if any(widget["header bounds"]) else None),
        ("footer_bounds", bounds_text(widget["footer bounds"]) if any(widget["footer bounds"]) else None),
        ("description", our_name(widget["extended description widget"])
         if widget["extended description widget"] else None),
    ]
    patch = port_settings.WIDGET_PATCHES.get(our_name(tag), {})
    for key, value in patch.get("set", {}).items():
        pairs = [(k, v) for k, v in pairs if k != key] + [(key, value)]
    lines = [f"{indent}<widget{attributes(pairs)}>"]
    inner = indent + "\t"
    for data in widget["game data inputs"]:
        name = inputs[data["function"]]
        lines.append(f"{inner}<data{attributes([('input', f'unwired {name}' if name in INCOMPATIBLE else name)])}/>")
    lines += [f"{inner}{line}" for line in patch.get("handlers", [])]
    for handler in widget["event handlers"] if "handlers" not in patch else []:
        flags = handler["flags"]
        target = our_name(handler["widget tag"]) if handler["widget tag"] else None
        handler_pairs = [("event", EVENTS[handler["event type"]])]
        if flags >> 7 & 1:
            name = functions[handler["function"]]
            handler_pairs.append(("run", f"unwired {name}" if name in INCOMPATIBLE else name))
        if flags >> 10 & 1:
            handler_pairs.append(("script", handler["script"]))
        elif handler["script"]:
            handler_pairs.append(("label", handler["script"]))
        for bit, key in ((3, "open"), (8, "replace"), (6, "focus")):
            if flags >> bit & 1:
                handler_pairs.append((key, target))
        if flags >> 1 & 1:
            handler_pairs += [("close", "other"), ("widget", target)]
        if flags >> 5 & 1:
            handler_pairs += [("reload", "other"), ("widget", target)]
        for bit, pair in ((0, ("close", "current")), (2, ("close", "all")), (4, ("reload", "self")),
                          (9, ("back", "true")), (11, ("branch", "true"))):
            if flags >> bit & 1:
                handler_pairs.append(pair)
        if handler["sound effect"]:
            handler_pairs.append(("sound", handler["sound effect"]))
        seen, unique = set(), []
        for key, value in handler_pairs:
            if key not in seen:
                seen.add(key)
                unique.append((key, value))
        lines.append(f"{inner}<on{attributes(unique)}/>")
    for item in widget["search and replace functions"]:
        function = REPLACE[item["replace function"]] if item["replace function"] < len(REPLACE) else None
        lines.append(f"{inner}<replace{attributes([('search', item['search string']), ('function', function)])}/>")
    for conditional in widget["conditional widgets"]:
        lines.append(f"{inner}<conditional{attributes([('widget', our_name(conditional['widget tag'])), ('if_failed', 'true' if conditional['flags'] & 1 else None)])}/>")
    for child in widget["child widgets"]:
        child_tag = child["widget tag"]
        if child_tag in LEFT_OUT:
            continue
        x, y = CHILD_OFFSETS.get((tag, child_tag), (child["horizontal offset"], child["vertical offset"]))
        name = patch.get("swap", {}).get(our_name(child_tag), our_name(child_tag))
        lines += [f"{inner}{line}" for line in patch.get("insert_before", {}).get(our_name(child_tag), [])]
        pairs = [("widget", name), ("x", x or None), ("y", y or None),
                 ("controller", str(child["custom controller index"] + 1) if child["flags"] & 1 else None)]
        lines.append(f"{inner}<child{attributes(pairs)}/>")
    lines += [f"{inner}{line}" for line in patch.get("children", [])]
    lines.append(f"{indent}</widget>")
    return lines


def engine_names(source: Path, start: str, count: int) -> list:
    """the names a table of the game's functions gives them, in order"""
    import re
    text = source.read_text(encoding="latin-1")
    text = text[text.index(start):]
    return re.findall(r'"([^"\\]*)"', text[:text.index("}")])[:count]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--tags", type=Path, required=True)
    parser.add_argument("--definitions", type=Path, required=True)
    parser.add_argument("--redraws", type=Path, required=True)
    parser.add_argument("--placeholder", type=Path, required=True, help="the picture for frames with no redraw")
    parser.add_argument("--pictures", type=Path,
                        help="the PC version's pictures, as PNGs by tag path: only where each is, to place its placeholder")
    arguments = parser.parse_args()
    tags = Tags(arguments.tags, arguments.definitions)
    # (the names of the Xbox's own as the game has them, which the tags' own
    # names give a little differently: "create&edit", "NULL")
    functions = tags.options("UIEventHandlerReferenceFunction")
    inputs = tags.options("UIGameDataInputReferenceFunction")
    functions[:102] = engine_names(ROOT / "source/interface/ui_widget_event_handler_functions.c",
                                   '\t{\n\t\t"NULL",', 102)
    inputs[:41] = engine_names(ROOT / "port/linux/game/menu_tags.c", "game_data_input_names[] =", 41)
    for folder in (CE, SVG_COPIES, MENUS / "port_svg"):
        if folder.exists():
            shutil.rmtree(folder)
    CE.mkdir(parents=True)
    widgets = reachable(tags)
    # the sizes the widgets show their bitmaps at
    shown = defaultdict(list)
    for widget in widgets.values():
        top, left, bottom, right = widget["bounds"]
        size = (right - left, bottom - top)
        if widget["background bitmap"]:
            shown[widget["background bitmap"]].append(size)
        for kind in ("header", "footer"):
            if widget[f"list {kind} bitmap"]:
                top, left, bottom, right = widget[f"{kind} bounds"]
                shown[widget[f"list {kind} bitmap"]].append((right - left, bottom - top) if any(widget[f"{kind} bounds"])
                                                            else size)
    art = Art(tags, arguments.redraws, arguments.placeholder, shown, arguments.pictures)
    fonts = Counter()
    # the widgets, a file for each folder of their tags
    folders = defaultdict(list)
    for tag in sorted(widgets):
        folder = our_name(tag).rsplit("/", 1)[0] if "/" in our_name(tag) else "shell"
        # (the settings screens this port has its own of: tools/port_settings.py)
        if not port_settings.replaced(folder):
            folders[folder].append(tag)
    string_lists, bitmaps = set(), set()
    for widget in widgets.values():
        if widget["text label unicode strings list"]:
            string_lists.add(widget["text label unicode strings list"])
        for key in ("background bitmap", "list header bitmap", "list footer bitmap"):
            if widget[key] and widget[key] not in XBOX_BITMAPS:
                bitmaps.add(widget[key])
    for folder, members in sorted(folders.items()):
        lines = ['<?xml version="1.0" encoding="UTF-8"?>',
                 f"<!-- The PC version's menus: ui\\shell\\{folder.replace('/', chr(92))} (tools/ce_menus.py) -->"]
        lines.append("<menus>" if MAIN_MENU not in members else f"<menus{attributes([('root', our_name(MAIN_MENU))])}>")
        for tag in members:
            lines += widget_xml(tag, widgets[tag], tags, functions, inputs, fonts, {})
        lines.append("</menus>")
        (CE / f"{folder.replace('/', '.')}.xml").write_text("\n".join(lines) + "\n")
    for name, lines in [*port_settings.settings_files().items(), *port_settings.multiplayer_files().items()]:
        (CE / name).write_text("\n".join(lines))
    # their strings
    lines = ['<?xml version="1.0" encoding="UTF-8"?>',
             "<!-- The PC version's menus' text (tools/ce_menus.py) -->", "<menus>"]
    for tag in sorted(string_lists):
        strings = [text_value(item["string"]) for item in tags.load(tag, "unicode_string_list")["strings"]]
        strings = port_settings.STRING_OVERRIDES.get(our_name(tag), strings)
        for index, texts in port_settings.STRING_INSERTS.get(our_name(tag), []):
            strings = strings[:index] + texts + strings[index:]
        lines.append(f"\t<strings{attributes([('name', our_name(tag))])}>")
        lines += [f"\t\t<string{attributes([('text', text)])}/>" for text in strings]
        lines.append("\t</strings>")
    lines.append("</menus>")
    (CE / "strings.xml").write_text("\n".join(lines) + "\n")
    # their bitmaps
    lines = ['<?xml version="1.0" encoding="UTF-8"?>',
             "<!-- The PC version's menus' bitmaps, drawn from the redraws, else the PC version's own pictures"
             " (NON_HANDDRAWN.md; tools/ce_menus.py) -->", "<menus>"]
    lines += [art.bitmap(tag) for tag in sorted(bitmaps)]
    lines += [art.title(name, text) for name, text in sorted(port_settings.TITLES.items())]
    lines.append("</menus>")
    (CE / "bitmaps.xml").write_text("\n".join(lines) + "\n")
    # what is not hand-drawn
    report = [
        "# Menu pictures that are not redraws",
        "",
        "The menus' bitmaps are drawn from the high-res redraws (`svg/`, from ui-svg-handmade) where there is",
        "one. The PC version's pictures that have none (3D renders, screenshots and logos) are not in this",
        "repository.",
        "",
        "## From the Xbox's map",
        "",
        "These have the same pictures, in the same order, in the Xbox's `ui.map`, which the menus draw them",
        "from (the player's own; nothing is shipped). `mp_map_grafix`: the Xbox's has the 13 Xbox maps' pictures",
        "(the PC version's first 13) and its \"?\"; the PC version's other 8 are its own maps'.",
        "",
    ]
    report += [f"- `{tag}`" for tag in sorted(XBOX_BITMAPS)]
    report += [
        "",
        "These frames are drawn from a frame of the Xbox map's bitmap of the same name, whose other frames differ:",
        "the same picture, or for the profile settings the Xbox's picture for the same thing.",
        "",
        "| The PC version's frame | The Xbox's frame |",
        "| --- | --- |",
    ]
    report += [f"| `{name}.png` | `{tag}` frame {index} |" for name, tag, index in sorted(art.xbox_frames)]
    report += [
        "",
        "## Placeholders, to be redrawn",
        "",
        "The Xbox's map has none of these (or other pictures under the name), so each frame is a placeholder",
        "until it is redrawn.",
        "",
        "| File | Size | The PC version's picture |",
        "| --- | --- | --- |",
    ]
    report += [f"| `{png}` | {w}x{h} | `{source}` |" for png, w, h, source in sorted(art.pictures)]
    report += ["", f"{len(art.pictures)} of {len(art.pngs)} frames are placeholders.",
               "", "The PC version's `ui\\gamespy` and `ui\\ticker` fonts are drawn with `ui\\small_ui`, which",
               "the Xbox's map has.", ""]
    (MENUS / "NON_HANDDRAWN.md").write_text("\n".join(report))
    # what is not wired
    used = Counter()
    for path in CE.glob("*.xml"):
        import re
        for name in re.findall(r'(?:run|input)="([^"]*)"', path.read_text()):
            used[name] += 1
    pc_functions = set(functions[102:]) | set(inputs[41:])
    unwired = sorted((name[8:] if name.startswith("unwired ") else name, count) for name, count in used.items()
                     if name.startswith("unwired ") or (name in pc_functions and name not in WIRED))
    report = [
        "# Menu functions that are not wired yet",
        "",
        "The PC version's menus (`ce/`, from `tools/ce_menus.py`) run functions of the game's. These do nothing",
        "yet (`port/linux/game/menu_functions.c`), so what they fill in (lists, values) is empty, and what they",
        "do (save a setting, join a game) does not happen; the screens open and close as the PC version's.",
        "",
        "- The PC version's own functions, which the Xbox's game has not.",
        "- The Xbox's functions that the PC version rewrote for its own widgets, written `unwired <name>` in the",
        "  files: run on the PC widgets, the Xbox's would stop the game.",
        "",
        "| Function | Uses | |",
        "| --- | --- | --- |",
    ]
    report += [f"| `{name}` | {count} | {'Xbox function, PC widgets' if name in INCOMPATIBLE else 'PC function'} |"
               for name, count in unwired]
    report += ["", "Wired: " + ", ".join(f"`{name}`" for name in sorted(WIRED)) + ".", ""]
    (MENUS / "UNWIRED.md").write_text("\n".join(report))
    files = sorted(path.relative_to(MENUS).as_posix() for path in MENUS.rglob("*.xml"))
    files += sorted(art.pngs)
    (MENUS / "menus.json").write_text(json.dumps({
        "comment": "The menus' files the game embeds (tools/ce_menus.py, tools/embed_assets.py).",
        "files": files,
    }, indent=1) + "\n")
    size = sum((MENUS / name).stat().st_size for name in files)
    print(f"{len(widgets)} widgets in {len(folders)} files, {len(string_lists)} string lists, {len(bitmaps)} bitmaps "
          f"({len(art.pngs)} frames, {len(art.pictures)} not redrawn); {size // 1024} KB; fonts {dict(fonts)}")


if __name__ == "__main__":
    main()
