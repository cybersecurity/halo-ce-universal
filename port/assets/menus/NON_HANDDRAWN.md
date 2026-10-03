# Menu pictures that are not redraws

The menus' bitmaps are drawn from the high-res redraws (`svg/`, from ui-svg-handmade) where there is
one. The PC version's pictures that have none (3D renders, screenshots and logos) are not in this
repository.

## From the Xbox's map

These have the same pictures, in the same order, in the Xbox's `ui.map`, which the menus draw them
from (the player's own; nothing is shipped). `mp_map_grafix`: the Xbox's has the 13 Xbox maps' pictures
(the PC version's first 13) and its "?"; the PC version's other 8 are its own maps'.

- `ui\shell\bitmaps\colors_sm`
- `ui\shell\bitmaps\mp_map_grafix`
- `ui\shell\bitmaps\sp_levels`
- `ui\shell\main_menu\difficulty_select\difficulty_options`
- `ui\shell\main_menu\difficulty_select\difficulty_options_small`
- `ui\shell\main_menu\halo_logo`
- `ui\shell\main_menu\settings_select\player_setup\player_profile_edit\color_edit\player_color_marine_large`

These frames are drawn from a frame of the Xbox map's bitmap of the same name, whose other frames differ:
the same picture, or for the profile settings the Xbox's picture for the same thing.

| The PC version's frame | The Xbox's frame |
| --- | --- |
| `shell/main_menu/multiplayer_type_select/mp_options__2.png` | `ui\shell\main_menu\multiplayer_type_select\mp_options` frame 3 |
| `shell/main_menu/settings_select/multiplayer_setup/playlist_edit/gametype_options__2.png` | `ui\shell\main_menu\settings_select\multiplayer_setup\playlist_edit\gametype_options` frame 2 |
| `shell/main_menu/settings_select/multiplayer_setup/playlist_edit/gametype_options__5.png` | `ui\shell\main_menu\settings_select\multiplayer_setup\playlist_edit\gametype_options` frame 4 |
| `shell/main_menu/settings_select/player_setup/player_profile_edit/profile_options__0.png` | `ui\shell\main_menu\settings_select\player_setup\player_profile_edit\profile_options` frame 0 |
| `shell/main_menu/settings_select/player_setup/player_profile_edit/profile_options__2.png` | `ui\shell\main_menu\settings_select\player_setup\player_profile_edit\profile_options` frame 1 |
| `shell/main_menu/settings_select/player_setup/player_profile_edit/profile_options__7.png` | `ui\shell\main_menu\settings_select\player_setup\player_profile_edit\profile_options` frame 3 |

## Placeholders, to be redrawn

The Xbox's map has none of these (or other pictures under the name), so each frame is a placeholder
until it is redrawn.

| File | Size | The PC version's picture |
| --- | --- | --- |
| `ce/shell/main_menu/multiplayer_type_select/mp_options__0.png` | 512x256 | `shell/main_menu/multiplayer_type_select/mp_options__0.png` |
| `ce/shell/main_menu/multiplayer_type_select/mp_options__1.png` | 512x256 | `shell/main_menu/multiplayer_type_select/mp_options__1.png` |
| `ce/shell/main_menu/settings_select/multiplayer_setup/playlist_edit/gametype_options__0.png` | 512x256 | `shell/main_menu/settings_select/multiplayer_setup/playlist_edit/gametype_options__0.png` |
| `ce/shell/main_menu/settings_select/multiplayer_setup/playlist_edit/gametype_options__3.png` | 512x256 | `shell/main_menu/settings_select/multiplayer_setup/playlist_edit/gametype_options__3.png` |
| `ce/shell/main_menu/settings_select/multiplayer_setup/playlist_edit/gametype_options__4.png` | 512x256 | `shell/main_menu/settings_select/multiplayer_setup/playlist_edit/gametype_options__4.png` |
| `ce/shell/main_menu/settings_select/multiplayer_setup/playlist_edit/gametype_options__6.png` | 512x256 | `shell/main_menu/settings_select/multiplayer_setup/playlist_edit/gametype_options__6.png` |
| `ce/shell/main_menu/settings_select/multiplayer_setup/playlist_edit/gametype_options__7.png` | 512x256 | `shell/main_menu/settings_select/multiplayer_setup/playlist_edit/gametype_options__7.png` |
| `ce/shell/main_menu/settings_select/player_setup/player_profile_edit/profile_options__1.png` | 512x256 | `shell/main_menu/settings_select/player_setup/player_profile_edit/profile_options__1.png` |
| `ce/shell/main_menu/settings_select/player_setup/player_profile_edit/profile_options__3.png` | 512x256 | `shell/main_menu/settings_select/player_setup/player_profile_edit/profile_options__3.png` |
| `ce/shell/main_menu/settings_select/player_setup/player_profile_edit/profile_options__4.png` | 512x256 | `shell/main_menu/settings_select/player_setup/player_profile_edit/profile_options__4.png` |
| `ce/shell/main_menu/settings_select/player_setup/player_profile_edit/profile_options__5.png` | 512x256 | `shell/main_menu/settings_select/player_setup/player_profile_edit/profile_options__5.png` |
| `ce/shell/main_menu/settings_select/player_setup/player_profile_edit/profile_options__6.png` | 512x256 | `shell/main_menu/settings_select/player_setup/player_profile_edit/profile_options__6.png` |
| `ce/shell/main_menu/settings_select/player_setup/player_profile_edit/profile_options__8.png` | 512x256 | `shell/main_menu/settings_select/player_setup/player_profile_edit/profile_options__8.png` |

13 of 147 frames are placeholders.

The PC version's `ui\gamespy` and `ui\ticker` fonts are drawn with `ui\small_ui`, which
the Xbox's map has.
