# Menus

The game's menus are the PC version's (Halo Custom Edition's): its main menu
and every screen it leads to. They are XML files in this folder, which the
game builds into its own widget tags when the menus' map (`ui.map`) loads
(`port/linux/game/menu_tags.c`). They then work like the Xbox's menus: with a
controller, the keyboard and the mouse, on every port.

| Path | Contents |
| --- | --- |
| `ce/*.xml` | The PC version's screens, one file for each folder of its tags, laid out as it has them; `ce/strings.xml` their text, `ce/bitmaps.xml` their pictures |
| `ce/shell/...png` | The pictures, drawn from the high-res redraws in `svg/` (up to 4x), else a placeholder |
| `NON_HANDDRAWN.md` | The pictures that are not redraws: those drawn from the Xbox's map (the player's own), and the placeholders, to be redrawn |
| `UNWIRED.md` | The menus' functions that do nothing yet: the lists they fill are empty, and the settings they change do not change |
| `menus.json` | The files the game embeds |

`tools/ce_menus.py` writes all of them from the PC version's tags and the
redraws, but the settings screens (Controls Setup, Gamepads, Mouse, Audio,
Video and Network Setup), which `tools/port_settings.py` writes in their
style with what this port has to set: config.toml's settings (a window or
the full screen, the frame rate, the volumes, internet play, the
multiplayer HUD), the profile's controller settings, and the keyboard and
mouse's controls.

To change the menus without building the game, put files in a `menus` folder
next to `config.toml`. A file with the same path as one here replaces it. On
the desktop, any other `.xml` file there is added too. A PNG that a `<frame>`
names is found there first. If any file has a problem, the log names the
file, the line and the problem, and the game uses the Xbox's menus. The
setting `display.menus = "xbox"` also uses them. `debug.menu_open` starts on
one screen (by its name, `main_menu/settings_select/...`), for looking at it.

## Elements

A file holds one `<menus>` element (`root` names the main menu's widget, in
one file; `main_menu` if none does). It holds `<bitmap>`, `<strings>` and
`<widget>` elements. All files share one set of names. Wherever a name is
asked for, one with a backslash is a tag of the map instead
(`ui\shell\bitmaps\white`).

### `<bitmap>`

```xml
<bitmap name="bitmaps/list_item_bkd">
	<frame png="ce/shell/bitmaps/list_item_bkd__0.png" width="256" height="32"/>
	<frame png="ce/shell/bitmaps/list_item_bkd__1.png" width="256" height="32"/>
</bitmap>
```

A picture for widgets. Each `<frame>` is an 8-bit RGBA PNG and its size in
the 640x480 menu screen (the PNG is a whole multiple of it), or a frame of a
bitmap of the map: `<frame map="ui\shell\...\profile_options" index="1"/>`. (Or
`frames="a.png b.png"` with one `width` and `height` for all.) A widget draws
its bitmap from the top left, one unit to a texel, cut to the widget's size.
A list's item shows frame 1 while it has the focus and frame 0 otherwise, if
the bitmap has exactly two frames; the game's functions choose others.

### `<strings>`

```xml
<strings name="main_menu/main_menu_options">
	<string text="NEW CAMPAIGN"/>
	<string text="LOAD CAMPAIGN"/>
</strings>
```

A list of texts, which widgets show by number. `\n` starts a new line.

### `<widget>`

A screen, or a part of one. Its children are `<widget>` elements in it, or
`<child widget="name" x="" y=""/>` for one defined elsewhere (a widget can
be a child of many).

| Attribute | Value |
| --- | --- |
| `name` | Its name, unique (the game knows it as `pc\<name>`, and shows the last part) |
| `type` | `container` (the default), `text`, `spinner` or `column_list` |
| `x`, `y` | Where it is in its parent (a `<widget>` inside another) |
| `left`, `top`, `width`, `height` | Its area; 0, 0, 640 and 480 if not given |
| `flags` | Widget flags (below), separated by spaces |
| `bitmap` | Its background: a `<bitmap>` |
| `string_list`, `string_index` | Its text: a `<strings>` and the number in it |
| `text` | Its text, given here |
| `font` | `large`, `small`, `terminal`, or a font tag of the map |
| `color` | The text's colour, `#RRGGBB` or `#AARRGGBB`. Focused text is white. |
| `align`, `text_x`, `text_y` | `left`, `right` or `center`, and where the text starts in it |
| `text_flags` | `editable`, `password`, `flashing`, `no_focus_test` |
| `list_flags` | `items_in_code`, `items_from_strings` (a spinner showing its strings), `one_tooltip`, `single_preview` |
| `strings`, `setting`, `values` | A spinner's values as shown, separated by `\|`; its `config.toml` setting and the values written for each (`port setting load` and `port setting save`) |
| `header_bitmap`, `footer_bitmap`, `header_bounds`, `footer_bounds` | A list's header and footer: a `<bitmap>` and "top left bottom right" |
| `description` | A column list's extended description: a widget |
| `controller` | `1` to `4`, or `any` (the default); a `<child>`'s or `child_controller`: the one it is for |
| `auto_close`, `auto_close_fade` | It closes itself after this many milliseconds, fading for this many |
| `platform` | `desktop` or `android`: it is only there. Every element takes this. |

Widget flags: `pass_unhandled_to_focused_child` (an event it does not handle
goes to its focused child: a screen needs it for its list to get events),
`pass_unhandled_to_all_children`, `pass_handled_to_all_children`,
`up_down_tabs_items`, `left_right_tabs_items`, `up_down_tabs_children`,
`left_right_tabs_children`, `no_focused_child`, `pause_game`, `flash_bitmap`,
`render_any_controller`, `main_menu_if_no_history`, `tag_controller_index`,
`nifty_fx`, `no_history`, and the PC version's `force_handle_mouse` and
`no_widescreen_fill`, which this engine has no use for.

A widget takes the focus if it has an `<on>` or is a list; a screen gives it
to its first such child.

### `<on>`

What a widget does for an event: `event` (one or more, separated by spaces)
and what to do.

| Attribute | Value |
| --- | --- |
| `event` | `a`, `b`, `x`, `y`, `black`, `white`, `left_trigger`, `right_trigger`, `up`, `down`, `left`, `right`, `start`, `back`, `left_thumb`, `right_thumb`, `stick_up`, `stick_down`, `stick_left`, `stick_right`, `right_stick_up`, `right_stick_down`, `right_stick_left`, `right_stick_right`, `created`, `deleted`; the PC version's `get_focus`, `lose_focus`, `left_mouse`, `middle_mouse`, `right_mouse`, `double_click`, `custom_activation` and `post_render`, which this engine never sends (but `custom_activation`: `emit custom activation event` and `single prev cl item activated` send it) |
| `run` | A function: the game's, as its tags name them; the PC version's; the port's (below); or `unwired <name>`, which does nothing (`UNWIRED.md`) |
| `open`, `replace`, `focus` | A widget to open, to open in its place, to give the focus |
| `back` | `true`: back to the screen before |
| `close` | `current`, `all`, or `other` (with `widget`) |
| `reload` | `self`, or `other` (with `widget`) |
| `branch` | `true`: if `run` fails, open its `<conditional>` widget |
| `otherwise` | A widget to open if `run` fails (a `<conditional>` and `branch` in one) |
| `script` | A script of the menus' scenario to run |
| `label` | Kept as the script's name, not run (as the tags keep it) |
| `sound` | A sound tag of the map to play |

On the keyboard, A is Enter or Space, B is Backspace or F, Start is Escape and
Back is F1. The mouse picks the item under it, clicks A, right-clicks B and
scrolls lists.

The port's functions: `port quit game` quits; `port setting load` (on a
spinner's `created`) shows its setting's value and `port setting save` (on
its `deleted`) writes the value shown to `config.toml`, if it changed. Of
the PC version's: `main menu quit game` quits, `profile set edit begin`
begins editing the first player profile, `mouse emit back event` and
`mouse emit x event` push B and X, `emit custom activation event` and
`single prev cl item activated` run the screen's or the list's
`custom_activation` handlers, and its back handlers go back. Its
campaign's play: `campaign menu continue` (the saved game), New Game's list
of levels and the difficulty menu (`difficulty item select`, and `set
difficulty` on its OK button: the difficulty shown), and Load Game's list of
the profiles' saved games (with `load game menu delete finish`). For our
widgets, `initialize sp level list solo`, `solo level set map` and `set
difficulty` are the port's, as the Xbox's take the Xbox's widgets. The
settings screens': `port settings save` (OK) writes the settings changed
(config.toml's, or `profile.<field>`: the profile being edited) and applies
them, `port settings defaults` shows the defaults, `port settings help` the
help of the row chosen; Controls Setup's `controls ...` bind the keyboard
and mouse's controls (Enter takes the next key or button pressed; left and
right choose which of two; Escape cancels, Delete clears); Change Color's
`color picker ...` (the Xbox's names) set the profile's colour. The others
do nothing yet.

### `<data>`, `<conditional>`, `<replace>`

`<data input="..."/>`: a game data function, which fills in the widget every
frame (`build number textbox only`). `<conditional widget="..."
if_failed="true"/>`: a widget for a `branch` handler. `<replace search="..."
function="controller"/>`: text replaced in the widget's (the controller's
number).

## The art

`tools/ce_menus.py` draws each frame from its redraw in `svg/` (copied from
ui-svg-handmade) at up to 4x its size. A picture with no redraw is the
Xbox's map's, where it has the same one (a whole bitmap, or a `<frame
map=...>`), else a placeholder, placed where the PC version's picture is in
its frame (`--pictures`: only where, not the picture, is read);
`NON_HANDDRAWN.md` lists both. It needs `rsvg-convert`, Pillow, NumPy and
SciPy.
