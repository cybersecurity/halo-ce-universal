# Menu functions that are not wired yet

The PC version's menus (`ce/`, from `tools/ce_menus.py`) run functions of the game's. These do nothing
yet (`port/linux/game/menu_functions.c`), so what they fill in (lists, values) is empty, and what they
do (save a setting, join a game) does not happen; the screens open and close as the PC version's.

- The PC version's own functions, which the Xbox's game has not.
- The Xbox's functions that the PC version rewrote for its own widgets, written `unwired <name>` in the
  files: run on the PC widgets, the Xbox's would stop the game.

| Function | Uses | |
| --- | --- | --- |
| `common button bar update` | 24 | PC function |
| `direct ip connect init` | 1 | PC function |
| `direct ip connect update` | 1 | PC function |
| `direct ip edit field` | 4 | PC function |
| `gamespy update filter settings` | 2 | PC function |
| `gt edit list update` | 1 | PC function |
| `gt select list update` | 1 | PC function |
| `mouse spinner 1wide click` | 66 | PC function |
| `mp map list update` | 1 | PC function |
| `mp prof init teamplay options` | 1 | PC function |
| `mp prof init vehicle options` | 1 | PC function |
| `mp prof save teamplay options` | 1 | PC function |
| `mp prof save vehicle options` | 1 | PC function |
| `mp prof vehicles update` | 1 | PC function |
| `server settings init` | 1 | PC function |
| `server settings update` | 1 | PC function |
| `ss edit server name` | 2 | PC function |
| `ss start game` | 2 | PC function |

Wired: `campaign menu continue`, `campaign menu init`, `controls back handler`, `controls begin binding`, `controls screen change set`, `controls screen defaults`, `controls screen init`, `controls update menu`, `difficulty item select`, `direct ip connect go`, `emit custom activation event`, `gamespy back handler`, `gamespy dismiss error`, `gamespy dismiss filters`, `gamespy screen dispose`, `gamespy screen init`, `gamespy screen update`, `gamespy select button`, `gamespy select header`, `gamespy select item`, `load game list update`, `load game menu activated`, `load game menu delete finish`, `load game menu delete request`, `load game menu dispose`, `load game menu init`, `main menu quit game`, `mouse emit accept event`, `mouse emit back event`, `mouse emit x event`, `mp type set mode`, `profile manager select`, `profile set edit begin`, `single prev cl item activated`, `solo map list update`.
