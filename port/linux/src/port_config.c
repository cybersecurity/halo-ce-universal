/*
PORT_CONFIG.C

The native ports' settings (port_config.h), parsed with tomlc17
(port/third_party/tomlc17). Every setting is in the table below with its
type, default, the HALO_* environment variable that overrides it and the
comment written into a new file. The file is read once, on the first
question; unknown keys and values of the wrong type are reported in the log
and the defaults used instead, and the file itself is never rewritten once
it exists, so that the player's edits and comments stay.
*/

#include "platform.h"
#include "port_config.h"
#include "tomlc17.h"

#include <SDL3/SDL.h>
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- the settings */

enum config_type
{
	_config_boolean,
	_config_integer,
	_config_real,
	_config_string,
};

/* how the setting's environment variable sets it */
enum config_environment
{
	/* the variable's text is the value ("0", "false", "no" and "off" are
	false for a boolean) */
	_environment_value,
	/* the variable being set (not empty, "0", "false", "no" or "off") makes it true */
	_environment_set_is_true,
	/* the variable being set (not empty, "0", "false", "no" or "off") makes it false */
	_environment_set_is_false,
};

/* the builds a setting means something in, and is written for */
enum
{
	_platform_desktop = 1,
	_platform_android = 2,
	_platform_all = _platform_desktop | _platform_android,
	/* (of the desktop builds, only Windows) */
	_platform_windows = 4,
	/* (the web build, which has the desktop's settings, and these) */
	_platform_web = 8,
};

struct config_setting
{
	const char *name;
	enum config_type type;
	/* as it is written in the file */
	const char *default_value;
	/* NULL: none, for a setting the Android app reads from the file itself */
	const char *environment;
	enum config_environment environment_style;
	unsigned platforms;
	const char *comment;
};

/* crouching's keys (in a browser C alone: a page cannot keep Ctrl+W, Ctrl+S or
Ctrl+D, which moving and crouching would press, from the browser) */
#ifdef HALO_WEB
#define CROUCH_KEYS "\"C\""
#else
#define CROUCH_KEYS "\"Left Ctrl, C\""
#endif

static const struct config_setting config_settings[] =
{
	{ "display.fullscreen", _config_boolean, "true", "HALO_FULLSCREEN", _environment_value, _platform_desktop,
		"Where display.mode is empty: start borderless over the whole display;\n"
		"false starts in a window. F11 switches." },
	{ "display.mode", _config_string, "\"\"", "HALO_DISPLAY_MODE", _environment_value, _platform_desktop,
		"\"fullscreen\" takes the display (at display.resolution's mode),\n"
		"\"borderless\" is a window over the whole desktop, \"windowed\" a window\n"
		"(display.window_size). Empty: display.fullscreen's (true: borderless).\n"
		"F11 switches to the window and back." },
	{ "display.resolution", _config_string, "\"native\"", "HALO_RESOLUTION", _environment_value, _platform_desktop,
		"What fullscreen and borderless draw at: \"native\", the display's own, or\n"
		"\"<width>x<height>\" (\"1920x1080\"), 640x480 or more. Fullscreen sets the\n"
		"display to it; borderless draws it scaled to the display." },
	{ "display.resolution_scaling", _config_string, "\"native\"", "HALO_RESOLUTION_SCALING", _environment_value,
		_platform_desktop,
		"\"native\" draws at the window's resolution (fullscreen, the display's or\n"
		"display.resolution); \"original\" draws the Xbox's 640x480 and scales it\n"
		"up." },
	{ "display.window_size", _config_string, "\"\"", "HALO_WINDOW_SIZE", _environment_value, _platform_desktop,
		"The window's size, \"<width>x<height>\" (\"1920x1080\"), 640x480 or more (it\n"
		"can be resized). Empty: display.window_scale's." },
	{ "display.window_scale", _config_integer, "2", "HALO_WINDOW_SCALE", _environment_value, _platform_desktop,
		"Where display.window_size is empty: the window's size as a multiple of\n"
		"640x480." },
	{ "display.screen_width", _config_integer, "0", "HALO_SCREEN_WIDTH", _environment_value, _platform_android,
		"Columns of the 480-line picture: 0 for the display's shape, 640 for the\n"
		"Xbox's 4:3." },
	{ "display.vsync", _config_boolean, "true", "HALO_NO_VSYNC", _environment_set_is_false, _platform_all,
		"Wait for the display between frames; false draws as fast as possible." },
	{ "display.max_fps", _config_integer, "0", "HALO_MAX_FPS", _environment_value, _platform_desktop,
		"With vsync off, the most frames a second: 0 for twice the display's\n"
		"refresh rate, -1 for no limit (which can hang some Intel graphics)." },
	{ "display.anti_aliasing", _config_string, "\"off\"", "HALO_ANTI_ALIASING", _environment_value, _platform_all,
		"Smoothing of jagged edges, which the Xbox did not have: \"off\"; \"fxaa\"\n"
		"or \"smaa\" smooth the 3D view once it is drawn (the HUD and menus stay\n"
		"sharp); \"ssaa2x\" draws at twice the resolution each way (four times\n"
		"the work); \"msaa2x\", \"msaa4x\" or \"msaa8x\" draw with that many samples\n"
		"a pixel. Android has \"fxaa\" for \"smaa\", and no \"ssaa2x\"." },
	{ "display.interpolation", _config_boolean, "true", "HALO_INTERPOLATION", _environment_value, _platform_all,
		"Draw a frame for every display refresh, blending between the game's 30\n"
		"ticks a second; false keeps the original 30 frames a second." },
	{ "display.direct_camera", _config_boolean, "true", "HALO_DIRECT_CAMERA", _environment_value, _platform_desktop,
		"In first person, point the view where the player aims now instead of\n"
		"where the last tick left it: the view turns the frame the mouse moves,\n"
		"not up to two ticks (66 ms) later." },
	{ "display.fov", _config_real, "0.0", "HALO_FOV", _environment_value, _platform_all,
		"The first-person view's field of view on foot, in degrees across at\n"
		"16:9 (20 to 150); 0 keeps the stock view. Vehicles, cinematics and\n"
		"scripted cameras keep their own." },
	{ "display.viewmodel_fov", _config_real, "0.0", "HALO_VIEWMODEL_FOV", _environment_value, _platform_all,
		"The first-person weapon's and hands' field of view, in degrees across\n"
		"at 16:9 (20 to 150); 0 keeps the weapon's stock view, also when\n"
		"display.fov widens the world." },
	{ "display.viewmodel_visible", _config_boolean, "true", "HALO_VIEWMODEL_VISIBLE", _environment_value, _platform_all,
		"Draw the first-person weapon, hands and what is attached to them.\n"
		"Off, they are not drawn; firing, animation, sound and lights go on." },
	{ "display.high_res_hud", _config_boolean, "true", "HALO_HIGH_RES_HUD", _environment_value, _platform_all,
		"Draw the HUD (meters, counters, panels, motion sensor, reticles,\n"
		"waypoints, scopes) from the high-res assets (8x the maps' bitmaps);\n"
		"false draws the maps' own bitmaps." },
	{ "display.high_res_text", _config_boolean, "true", "HALO_HIGH_RES_TEXT", _environment_value, _platform_all,
		"Draw the menus' and HUD's text with the fonts in port/assets/fonts\n"
		"(Overpass) at the resolution the game draws at, and the menus' titles\n"
		"from port/assets/titles; false draws the maps' bitmap fonts and titles." },
	{ "display.shadow_resolution", _config_integer, "128", "HALO_SHADOW_RESOLUTION", _environment_value,
		_platform_all,
		"The size the objects' shadows are drawn at, in pixels each way: 128 as\n"
		"on the Xbox, or 256, 512 or 1024 for smoother edges, as soft." },
	{ "display.menus", _config_string, "\"pc\"", "HALO_MENUS", _environment_value, _platform_all,
		"The menus: \"pc\" for the PC version's main menu (port/assets/menus,\n"
		"and a menus folder here for your own), \"xbox\" for the Xbox's." },
	{ "display.player_names", _config_string, "\"all\"", "HALO_PLAYER_NAMES", _environment_value, _platform_all,
		"In multiplayer, whose names are drawn above their heads: \"all\",\n"
		"\"allies\", \"enemies\" or \"none\". An enemy's shows only within the\n"
		"motion sensor's reach, in sight and not camouflaged; none show if the\n"
		"gametype's motion tracker shows no players, only allies' if it shows\n"
		"only friends." },
	{ "display.player_name_scale", _config_real, "1.0", "HALO_PLAYER_NAME_SCALE", _environment_value, _platform_all,
		"How large the players' names are drawn: 1.0 three quarters of the size of\n"
		"the HUD's text, 0.25 to 4." },
	{ "display.scoreboard_team_layout", _config_string, "\"teams\"", "HALO_SCOREBOARD_TEAM_LAYOUT", _environment_value,
		_platform_all,
		"How the scoreboard lists a team game's players: \"teams\" in a column for\n"
		"each team (red on the left, blue on the right), \"score\" all in order of\n"
		"score." },
	{ "display.scoreboard_background", _config_boolean, "true", "HALO_SCOREBOARD_BACKGROUND", _environment_value,
		_platform_all,
		"Draw a panel behind the multiplayer scoreboard, for clearer text." },
	{ "display.scoreboard_background_color", _config_string, "\"16, 16, 16, 150\"", "HALO_SCOREBOARD_BACKGROUND_COLOR",
		_environment_value, _platform_all,
		"The scoreboard panel's colour: \"red, green, blue, alpha\", each 0 to 255\n"
		"(alpha 0 is see-through, 255 solid)." },
	{ "display.per_pixel_lighting", _config_boolean, "false", "HALO_PER_PIXEL_LIGHTING", _environment_value,
		_platform_all,
		"Light the models (characters, weapons, vehicles, scenery) for each\n"
		"pixel by the lights the game gives them, without the facets the light\n"
		"of each vertex shows across curved surfaces; false lights each vertex,\n"
		"as the Xbox does." },

	{ "audio.enabled", _config_boolean, "true", "HALO_NO_AUDIO", _environment_set_is_false, _platform_all,
		"Play sound." },
	{ "audio.volume", _config_real, "1.0", "HALO_VOLUME", _environment_value, _platform_all,
		"The volume of everything, 0.0 to 1.0." },
	{ "audio.music_volume", _config_real, "1.0", "HALO_MUSIC_VOLUME", _environment_value, _platform_all,
		"The music's volume, 0.0 to 1.0 (of audio.volume)." },
	{ "audio.effects_volume", _config_real, "1.0", "HALO_EFFECTS_VOLUME", _environment_value, _platform_all,
		"The volume of every other sound (effects and speech), 0.0 to 1.0 (of\n"
		"audio.volume)." },
	{ "audio.reverb", _config_boolean, "true", "HALO_REVERB", _environment_value, _platform_all,
		"Reverberate the world's sounds as the place the player is in does (the\n"
		"maps' sound environments, as the Xbox's I3DL2 reverb did); false keeps\n"
		"them dry." },
	{ "audio.resampler", _config_string, "\"sinc\"", "HALO_AUDIO_RESAMPLER", _environment_value, _platform_all,
		"The sample-rate conversion mode:\n"
		"\"sinc\" (the default) for band-limited interpolation,\n"
		"or \"linear\" for a less filtered, brighter alternative." },
	{ "audio.voice_chat", _config_string, "\"push_to_talk\"", "HALO_VOICE_CHAT", _environment_value, _platform_all,
		"Talking in network games' voice chat: \"push_to_talk\" (while\n"
		"controls.push_to_talk is held; the microphone opens the first time),\n"
		"\"open_mic\" (whenever the microphone hears speech), or \"off\". Others'\n"
		"voices play whatever this is (audio.voice_volume 0 silences them)." },
	{ "audio.voice_volume", _config_real, "1.0", "HALO_VOICE_VOLUME", _environment_value, _platform_all,
		"The volume of the other players' voices (0 to 2)." },
	{ "audio.output_device", _config_string, "\"default\"", "HALO_AUDIO_OUTPUT_DEVICE", _environment_value,
		_platform_desktop,
		"Where the game's sound and the voices play: a device's name as Settings >\n"
		"Audio lists it, or \"default\" for the system's (also if it is not found)." },
	{ "audio.input_device", _config_string, "\"default\"", "HALO_AUDIO_INPUT_DEVICE", _environment_value,
		_platform_desktop,
		"The microphone voice chat listens to: a device's name as Settings >\n"
		"Audio lists it, or \"default\" for the system's (also if it is not found)." },
	{ "audio.loose_sounds", _config_boolean, "false", "HALO_LOOSE_SOUNDS", _environment_value, _platform_all,
		"For those making sounds: play each of a map's sounds that has a Halo PC\n"
		"sound tag file of its name under the data root's tags folder\n"
		"(tags/sound/.../name.sound) from that file. At the console,\n"
		"loose_sounds_reload reads the files again and loose_sounds false gives\n"
		"the map's sounds back." },

	{ "input.touch_controls", _config_string, "\"auto\"", "HALO_TOUCH_CONTROLS", _environment_value, _platform_android | _platform_web,
		"The on-screen touch controls in a game: \"auto\" shows them on a\n"
		"touchscreen while no controller is connected, \"on\" also with a\n"
		"controller, \"off\" never. A device without a touchscreen never shows\n"
		"them. The menus take taps in any case." },
	{ "input.touch_aim_assist", _config_boolean, "true", "HALO_TOUCH_AIM_ASSIST", _environment_value, _platform_android | _platform_web,
		"The touch controls' swipe aiming gets a controller's aim assist: the\n"
		"aim slows over a target and follows a moving one. false: none, as a\n"
		"mouse (the bullets' own autoaim stays)." },
	{ "input.mouse_sensitivity", _config_real, "1.0", "HALO_MOUSE_SENSITIVITY", _environment_value, _platform_desktop,
		"How far the view turns for the mouse's movement." },
	{ "input.invert_mouse", _config_boolean, "false", "HALO_MOUSE_INVERT", _environment_set_is_true, _platform_desktop,
		"Moving the mouse forward looks down." },
	{ "input.mouse_aim_assist", _config_boolean, "false", "HALO_MOUSE_AIM_ASSIST", _environment_value, _platform_desktop,
		"Magnetism while aiming with the mouse, as with a controller: the view\n"
		"slowed and dragged along by a target. The last of the mouse and the\n"
		"right stick to move decides. The bullets' autoaim (bent toward the\n"
		"target) stays either way." },
	{ "input.mouse_vertical_sensitivity", _config_real, "0.0", "HALO_MOUSE_VERTICAL_SENSITIVITY", _environment_value,
		_platform_desktop,
		"How far the view turns up and down for the mouse's movement; 0 for the\n"
		"same as input.mouse_sensitivity." },

	/* the keyboard and mouse's own controls (port/linux/src/xinput_sdl.c) */
	{ "controls.move_forward", _config_string, "\"W\"", "HALO_KEY_MOVE_FORWARD", _environment_value, _platform_all,
		"The keyboard and mouse's controls, which Settings > Controls Setup\n"
		"changes: up to two keys or buttons each, separated by a comma. Keys by\n"
		"their names (\"W\", \"Space\", \"Left Ctrl\", \"F1\"), and \"Mouse Left\",\n"
		"\"Mouse Right\", \"Mouse Middle\", \"Mouse 4\", \"Mouse 5\", \"Wheel\" (either\n"
		"way), \"Wheel Up\" and \"Wheel Down\"; empty for none. Moving forward:" },
	{ "controls.move_backward", _config_string, "\"S\"", "HALO_KEY_MOVE_BACKWARD", _environment_value, _platform_all,
		"Moving backward." },
	{ "controls.strafe_left", _config_string, "\"A\"", "HALO_KEY_STRAFE_LEFT", _environment_value, _platform_all,
		"Moving left." },
	{ "controls.strafe_right", _config_string, "\"D\"", "HALO_KEY_STRAFE_RIGHT", _environment_value, _platform_all,
		"Moving right." },
	{ "controls.jump", _config_string, "\"Space\"", "HALO_KEY_JUMP", _environment_value, _platform_all,
		"Jumping (and skipping cutscenes)." },
	{ "controls.crouch", _config_string, CROUCH_KEYS, "HALO_KEY_CROUCH", _environment_value, _platform_all,
		"Crouching." },
	{ "controls.fire", _config_string, "\"Mouse Left\"", "HALO_KEY_FIRE", _environment_value, _platform_all,
		"Firing." },
	{ "controls.throw_grenade", _config_string, "\"Mouse Right, G\"", "HALO_KEY_THROW_GRENADE", _environment_value,
		_platform_all,
		"Throwing a grenade." },
	{ "controls.melee", _config_string, "\"F, Mouse 4\"", "HALO_KEY_MELEE", _environment_value, _platform_all,
		"Melee attack." },
	{ "controls.reload", _config_string, "\"R\"", "HALO_KEY_RELOAD", _environment_value, _platform_all,
		"Reloading." },
	{ "controls.zoom", _config_string, "\"Z, Mouse Middle\"", "HALO_KEY_ZOOM", _environment_value, _platform_all,
		"Zooming the scope." },
	{ "controls.switch_weapon", _config_string, "\"Wheel, 1\"", "HALO_KEY_SWITCH_WEAPON", _environment_value,
		_platform_all,
		"Switching weapons." },
	{ "controls.switch_grenade", _config_string, "\"X\"", "HALO_KEY_SWITCH_GRENADE", _environment_value, _platform_all,
		"Switching grenades." },
	{ "controls.action", _config_string, "\"E\"", "HALO_KEY_ACTION", _environment_value, _platform_all,
		"The action: picking up (held: swapping weapons), entering and leaving\n"
		"vehicles, pressing switches; never reloading (the controller's X does\n"
		"when there is nothing to act on)." },
	{ "controls.flashlight", _config_string, "\"Q\"", "HALO_KEY_FLASHLIGHT", _environment_value, _platform_all,
		"The flashlight." },
	{ "controls.scoreboard", _config_string, "\"Tab\"", "HALO_KEY_SCOREBOARD", _environment_value, _platform_all,
		"Showing the scores (the controller's Back)." },
	{ "controls.pause", _config_string, "\"Escape\"", "HALO_KEY_PAUSE", _environment_value, _platform_all,
		"The pause menu (the controller's Start)." },
	{ "controls.screenshot", _config_string, "\"F10\"", "HALO_KEY_SCREENSHOT", _environment_value, _platform_all,
		"Save a PNG screenshot beside maps/ (press once per capture)." },
	{ "controls.push_to_talk", _config_string, "\"V\"", "HALO_KEY_PUSH_TO_TALK", _environment_value, _platform_all,
		"Voice chat: talk while it is held (audio.voice_chat \"push_to_talk\")." },

	{ "game.console_log", _config_string, "\"important\"", "HALO_CONSOLE_LOG", _environment_value, _platform_all,
		"What the game's console shows on screen of what it logs: \"important\"\n"
		"(bans, players dropped for cheating, what refuses a command, and the\n"
		"asserts that stop the game), \"all\" (every line, the game's own\n"
		"chatter too), or \"none\" (the asserts that stop the game only). What\n"
		"a command prints shows whatever this is, and debug.txt has every line." },

	{ "game.language", _config_string, "\"\"", "HALO_LANGUAGE", _environment_value, _platform_all,
		"The language the game asks the Xbox for: \"ja\", \"de\", \"fr\", \"es\" or \"it\";\n"
		"empty for English. The game data decides what is translated." },
	{ "game.enhanced_animations", _config_boolean, "true", "HALO_ENHANCED_ANIMATIONS", _environment_value, _platform_all,
		"The player bipeds' grenade throws keep their legs moving (crouched,\n"
		"in the air and in a vehicle's seat too), riders' hands leave the grips\n"
		"to throw and reload, and a player turns with the aim while throwing;\n"
		"false: the original animations, which freeze the legs and stand a\n"
		"rider up." },
	{ "game.custom_edition", _config_boolean, "true", "HALO_CUSTOM_EDITION", _environment_value, _platform_all,
		"Load and run Halo Custom Edition maps (not those that need OpenSauce):\n"
		"put them and Custom Edition's bitmaps.map, sounds.map and loc.map in\n"
		"the custom_maps folder beside the maps folder; the map lists show them\n"
		"as CUSTOM SINGLEPLAYER and CUSTOM MULTIPLAYER. Their tags are checked\n"
		"as the game's own maps' are before they run; false refuses them\n"
		"(docs/custom_edition_caches.md)." },
	{ "display.viewmodel_shield", _config_boolean, "false", "HALO_VIEWMODEL_SHIELD", _environment_value, _platform_all,
		"The energy shield's flare on the first-person arms too, as on the body\n"
		"(the stock game draws it on the body only)." },
	{ "display.first_person_legs", _config_boolean, "false", "HALO_FIRST_PERSON_LEGS", _environment_value, _platform_all,
		"Your own legs in first person, seen looking down: your body as others\n"
		"see it, drawn from the waist down, moving as it moves." },
	{ "display.shield_glow", _config_boolean, "false", "HALO_SHIELD_GLOW", _environment_value, _platform_all,
		"Energy shields light what is around them as they flare, as plasma\n"
		"does, in the shield's own color." },
	{ "display.shield_glow_intensity", _config_string, "\"default\"", "HALO_SHIELD_GLOW_INTENSITY", _environment_value, _platform_all,
		"How far the shield glow reaches: \"default\" or \"light_show\" (three\n"
		"times as far, and at full strength from a third of the flare)." },
	{ "display.shield_color", _config_string, "\"default\"", "HALO_SHIELD_COLOR", _environment_value, _platform_all,
		"Your energy shield's color, as this machine draws it: \"default\" (the\n"
		"shield's own) or a multiplayer armor color: white, black, red, blue,\n"
		"gray, yellow, green, pink, purple, cyan, cobalt, orange, teal, sage,\n"
		"brown, tan, maroon or salmon." },

	{ "paths.data", _config_string, "\"\"", "HALO_DATA_ROOT", _environment_value, _platform_desktop,
		"The folder holding the game data's maps folder; empty looks in the\n"
		"working directory and its assets folder. Windows paths are easiest in\n"
		"single quotes: 'C:\\Games\\Halo'." },
	{ "paths.saves", _config_string, "\"\"", "HALO_SAVE_ROOT", _environment_value, _platform_desktop,
		"Where saved games and profiles go; empty for the usual place\n"
		"(~/.local/share/halo-linux, or %APPDATA%\\halo on Windows)." },
	{ "paths.custom_edition", _config_string, "\"\"", "HALO_CUSTOM_EDITION_ROOT", _environment_value, _platform_desktop,
		"A Halo Custom Edition install whose maps folder is looked in after the\n"
		"custom_maps folder for Custom Edition maps and their bitmaps.map,\n"
		"sounds.map and loc.map (game.custom_edition); empty for none." },

	{ "network.address", _config_string, "\"\"", "HALO_NET_ADDRESS", _environment_value, _platform_all,
		"This machine's IPv4 address for system link, for a machine on several\n"
		"networks; empty chooses one." },
	{ "network.broadcast", _config_string, "\"\"", "HALO_NET_BROADCAST", _environment_value, _platform_all,
		"Comma-separated IPv4 addresses system link sends its announcements to\n"
		"instead of the local network's broadcast address (for VPNs); empty for\n"
		"the local network." },
	{ "network.online", _config_boolean, "true", "HALO_NET_ONLINE", _environment_value, _platform_all,
		"Internet play: hosting makes an invite link (logged, and put on the\n"
		"clipboard) that lets whoever has it join over the internet; opening a\n"
		"link (or copying one before switching to the game) joins. Only people\n"
		"with the invite can join. Off keeps system link to the local network." },
	{ "network.join_from_clipboard", _config_boolean, "true", "HALO_NET_JOIN_FROM_CLIPBOARD", _environment_value,
		_platform_all,
		"Join the game of an invite link found on the clipboard when the game\n"
		"comes to the front." },
	{ "network.tunnel_port", _config_integer, "0", "HALO_NET_TUNNEL_PORT", _environment_value, _platform_all,
		"The UDP port internet play uses; 0 picks one. A fixed one can be\n"
		"forwarded on the router, for networks whose NAT stops connections." },
	{ "network.allow_upnp", _config_boolean, "true", "HALO_NET_ALLOW_UPNP", _environment_value, _platform_all,
		"Let internet play ask the router (UPnP) to forward its port, for\n"
		"networks whose NAT stops connections: when a player joins this\n"
		"machine's game, and when joining a game takes too long. False never\n"
		"asks." },
	{ "network.public_lobby", _config_boolean, "true", "HALO_NET_PUBLIC_LOBBY", _environment_value, _platform_all,
		"The server browser: public games are listed through the signalling\n"
		"brokers, and Join Game > Server Browser shows them. False lists no\n"
		"game of this machine's and shows none." },
	{ "network.host_public", _config_boolean, "true", "HALO_NET_HOST_PUBLIC", _environment_value, _platform_all,
		"Whether a new game of Create Game > Internet starts as PUBLIC (listed\n"
		"in everyone's server browser: anyone can see and join it) or, false,\n"
		"PRIVATE (only players with its invite link can join). Server Setup's\n"
		"LISTING changes it for each game." },
	{ "network.voice_lobby", _config_boolean, "true", "HALO_NET_VOICE_LOBBY", _environment_value, _platform_all,
		"Hosting: voice chat in the lobby, before and after a game, where every\n"
		"player hears every other." },
	{ "network.voice_mode", _config_string, "\"team_global_enemy_proximity\"", "HALO_NET_VOICE_MODE",
		_environment_value, _platform_all,
		"Hosting: voice chat in a game: \"off\"; \"team_proximity\" (teammates\n"
		"near); \"team_enemy_proximity\" (anyone near); \"team_global\" (all\n"
		"teammates); or \"team_global_enemy_proximity\" (all teammates, and\n"
		"enemies near). A game without teams has only enemies; co-op only\n"
		"teammates." },
	{ "network.voice_kbps", _config_integer, "24", "HALO_NET_VOICE_KBPS", _environment_value, _platform_all,
		"Hosting: the voices' quality, in the lobby and in a game, in kilobits a\n"
		"second (8 to 64)." },
	{ "network.voice_proximity", _config_real, "15.0", "HALO_NET_VOICE_PROXIMITY", _environment_value,
		_platform_all,
		"Hosting: how near (world units: 1 is about 3 metres) a player must be\n"
		"to be heard by proximity voice chat (5 to 100)." },
	{ "network.votekick", _config_boolean, "true", "HALO_NET_VOTEKICK", _environment_value, _platform_all,
		"Hosting: let the players vote to kick a player (the scoreboard's\n"
		"right-click, or the console's votekick). More than half of the players\n"
		"must vote, counted once per address." },
	{ "network.votekick_minutes", _config_integer, "5", "HALO_NET_VOTEKICK_MINUTES", _environment_value,
		_platform_all,
		"Hosting: the minutes a player must have played on this server to start\n"
		"a vote to kick (0 to 60); to vote, 2 minutes or this, the less." },
	{ "network.votekick_ban_minutes", _config_integer, "30", "HALO_NET_VOTEKICK_BAN_MINUTES", _environment_value,
		_platform_all,
		"Hosting: the minutes a player kicked by a vote cannot join again (1 to\n"
		"1440)." },
	{ "network.coop_public", _config_boolean, "false", "HALO_NET_COOP_PUBLIC", _environment_value, _platform_all,
		"Whether an online co-op game (Create Game > Internet, a SINGLEPLAYER\n"
		"map) starts as PUBLIC or, false, PRIVATE: Server Setup's LISTING in\n"
		"co-op, which writes its choice here." },
	{ "network.coop_friendly_fire", _config_string, "\"on\"", "HALO_NET_COOP_FRIENDLY_FIRE", _environment_value,
		_platform_all,
		"Whether the players of an online co-op game hurt each other: \"off\",\n"
		"\"on\", \"shields_only\" or \"explosives_only\" (FRIENDLY FIRE in\n"
		"co-op's Server Setup > Co-op Options writes its choice here). Their AI\n"
		"allies they always can, as in the campaign." },
	{ "network.coop_player_collisions", _config_boolean, "true", "HALO_NET_COOP_PLAYER_COLLISIONS", _environment_value,
		_platform_all,
		"Whether the players of an online co-op game bump into each other;\n"
		"false, they walk through each other (the AI's characters they still\n"
		"bump into). PLAYER COLLISIONS in co-op's Server Setup > Co-op Options\n"
		"writes its choice here." },
	{ "network.coop_enemies_mode", _config_string, "\"per_player\"", "HALO_NET_COOP_ENEMIES_MODE", _environment_value,
		_platform_all,
		"Online co-op's extra enemies: \"none\", \"per_player\" (each squad of\n"
		"enemies grows by coop_enemies for each player past the first) or\n"
		"\"multiplier\" (each is coop_enemies_multiplier times as large, for any\n"
		"number of players). EXTRA ENEMIES in co-op's Server Setup > Co-op\n"
		"Options writes its choice here." },
	{ "network.coop_enemies", _config_integer, "50", "HALO_NET_COOP_ENEMIES", _environment_value, _platform_all,
		"Online co-op's extra enemies per player, a percentage: for each player\n"
		"past the first, each squad of enemies a level places gets this much of\n"
		"itself more (100: as many again; 25 to 200). PER PLAYER in co-op's\n"
		"Server Setup > Co-op Options writes its choice here." },
	{ "network.coop_enemies_multiplier", _config_integer, "2", "HALO_NET_COOP_ENEMIES_MULTIPLIER", _environment_value,
		_platform_all,
		"Online co-op's static multiplier of its enemies: each squad of enemies\n"
		"a level places is this many times as large (2 to 32). MULTIPLIER in\n"
		"co-op's Server Setup > Co-op Options writes its choice here." },
	{ "network.brokers_file", _config_string, "\"brokers.txt\"",
		"HALO_NET_BROKERS_FILE", _environment_value, _platform_all,
		"The file of the public MQTT brokers through which the machines of an\n"
		"invite find each other (its messages are encrypted), beside this file\n"
		"unless a full path: one host:port on each line, up to 4. Updates\n"
		"replace brokers.txt: keep a list of your own under another name." },
	{ "network.stun_servers", _config_string, "\"stun.l.google.com:19302,stun.cloudflare.com:3478\"",
		"HALO_NET_STUN", _environment_value, _platform_all,
		"Public STUN servers that tell this machine its internet address;\n"
		"comma-separated host:port." },
	{ "discord.application_id", _config_string, "\"1553978809840050229\"", "HALO_DISCORD_APPLICATION",
		_environment_value, _platform_desktop,
		"The Discord application internet play invites go through while the\n"
		"Discord desktop client runs; empty for none." },

	{ "update.auto", _config_boolean, "true", "HALO_UPDATE_AUTO", _environment_value, _platform_all,
		"Look for a new version when the game starts, and offer to update to it;\n"
		"false never looks (the game's \"Do not ask again\" writes false here)." },
	{ "crash_reports.upload", _config_string, "\"ask\"", "HALO_CRASH_REPORTS", _environment_value, _platform_windows,
		"Send a report of each crash (a minidump and halo.log) to the developers'\n"
		"Sentry project (port/windows/src/win32_crash.c): \"yes\" sends them, \"no\"\n"
		"never does, \"ask\" asks at the next crash and writes the answer here." },

	{ "debug.network_test", _config_string, "\"\"", "HALO_NETWORK_TEST", _environment_value, _platform_all,
		"Automated system link sessions for testing (port/linux/game/network_test.c):\n"
		"\"host:<map>\" hosts a game on that map, \"join\" joins the first game found,\n"
		"\"browse\" the one with the most players the server browser lists (not\n"
		"co-op, if it can); empty for none." },
	{ "debug.network_test_start", _config_real, "15.0", "HALO_NETWORK_TEST_START", _environment_value, _platform_all,
		"Seconds after hosting that an automated test game starts." },
	{ "debug.network_test_kill", _config_real, "0.0", "HALO_NETWORK_TEST_KILL", _environment_value, _platform_all,
		"Every this many seconds an automated test host kills its last player; 0 never." },
	{ "debug.network_test_score", _config_integer, "0", "HALO_NETWORK_TEST_SCORE", _environment_value, _platform_all,
		"The score an automated test host's game type plays to (a short game, to\n"
		"test the next); 0 the game type's own." },
	{ "debug.network_test_shoot", _config_real, "0.0", "HALO_NETWORK_TEST_SHOOT", _environment_value, _platform_all,
		"Every this many seconds each automated test player hits the next with\n"
		"their weapon, within its reach (the host brings far players near the\n"
		"first a second before); 0 never." },
	{ "debug.network_test_vehicle", _config_real, "0.0", "HALO_NETWORK_TEST_VEHICLE", _environment_value, _platform_all,
		"This many seconds into an automated test game the host seats its last\n"
		"player as a vehicle's driver (and out 15 seconds on); 0 never." },
	{ "debug.network_test_pickup", _config_real, "0.0", "HALO_NETWORK_TEST_PICKUP", _environment_value, _platform_all,
		"This many seconds into an automated test game the host stands its last\n"
		"player on a weapon, which a joining player then picks up; 0 never." },
	{ "debug.network_test_pickup_weapon", _config_string, "\"\"", "HALO_NETWORK_TEST_PICKUP_WEAPON", _environment_value,
		_platform_all,
		"The weapon network_test_pickup stands the player on: the first whose tag\n"
		"name has this in it (\"sniper\", say); empty any." },
	{ "debug.network_test_public", _config_boolean, "false", "HALO_NETWORK_TEST_PUBLIC", _environment_value,
		_platform_all,
		"The network test's host lists its game in the server browser, as Create\n"
		"Game > Internet's PUBLIC does (debug.network_test \"browse\" joins it)." },
	{ "debug.telnet_console", _config_boolean, "false", "HALO_TELNET_CONSOLE", _environment_set_is_true, _platform_all,
		"Listen on 127.0.0.1 (port telnet_console_port) for a script console that\n"
		"runs what it is sent as the game's console does, with no password; false\n"
		"none." },
	{ "debug.telnet_console_port", _config_integer, "2323", "HALO_TELNET_CONSOLE_PORT", _environment_value,
		_platform_all,
		"The port of the script console (telnet_console); the Xbox's was 23, which\n"
		"only the administrator can listen on." },
	{ "debug.touch_targets", _config_boolean, "false", "HALO_TOUCH_TARGETS", _environment_set_is_true, _platform_all,
		"Outline the menus' tap targets (item green, value blue, list slot yellow,\n"
		"legend button red, the band beside a list's slots orange; the virtual\n"
		"keyboard's keys white), mark where the last finger went down and the\n"
		"last tap landed for 3 seconds, and log each tap with the target it hit\n"
		"(and a value's split); to judge touch accuracy." },
	{ "debug.network_latency", _config_real, "0.0", "HALO_NETWORK_LATENCY", _environment_value, _platform_all,
		"Milliseconds everything received is held back (a round trip between two\n"
		"machines of twice it), to test the netcode as over the internet; 0 none." },
	{ "debug.network_loss", _config_real, "0.0", "HALO_NETWORK_LOSS", _environment_value, _platform_all,
		"Percent of datagrams received that are dropped, for the same; 0 none." },
	{ "debug.network_corrupt", _config_real, "0.0", "HALO_NETWORK_CORRUPT", _environment_value, _platform_all,
		"Percent of the datagrams received that are damaged at random, to test\n"
		"that nothing a machine sends can crash the game; 0 none." },
	{ "debug.network_corrupt_stream", _config_real, "0.0", "HALO_NETWORK_CORRUPT_STREAM", _environment_value,
		_platform_all,
		"Percent of the reads of streams that are damaged at random, for the\n"
		"same (a damaged stream is closed, so a little goes a long way); 0 none." },
	{ "debug.network_corrupt_after", _config_real, "0.0", "HALO_NETWORK_CORRUPT_AFTER", _environment_value,
		_platform_all,
		"Seconds after the start before anything is damaged, so that a game can\n"
		"be set up and started first (a host's messages to its own client are\n"
		"damaged too)." },
	{ "debug.test_input", _config_string, "\"\"", "HALO_TEST_INPUT", _environment_value, _platform_all,
		"\"bot:<seed>\" plays controller 1 with a scripted pattern (automated\n"
		"network tests); \"look:<seed>\" stands still, only turning and looking\n"
		"up and down; empty for none." },
	{ "debug.update_answer", _config_string, "\"\"", "HALO_UPDATE_ANSWER", _environment_value, _platform_desktop,
		"The answer to the new version question, for automated tests: \"yes\",\n"
		"\"no\" or \"never\" (do not ask again, confirmed); empty asks." },
	{ "debug.exit_after", _config_real, "0.0", "HALO_EXIT_AFTER", _environment_value, _platform_all,
		"Quit this many seconds after the window opens; 0 never." },
	{ "debug.hidden_window", _config_boolean, "false", "HALO_HIDDEN_WINDOW", _environment_set_is_true, _platform_desktop,
		"Keep the window hidden (and never fullscreen)." },
	{ "debug.voice_test", _config_boolean, "false", "HALO_VOICE_TEST", _environment_set_is_true, _platform_all,
		"Voice chat's automated tests: a tone instead of the microphone, and\n"
		"each voice heard logged once a second." },
	{ "debug.null_renderer", _config_boolean, "false", "HALO_NULL_RENDERER", _environment_set_is_true, _platform_all,
		"Run without a window, drawing nothing." },
	{ "debug.gl_debug", _config_boolean, "false", "HALO_GL_DEBUG", _environment_set_is_true, _platform_all,
		"Report OpenGL errors in the log." },
	{ "debug.menu_open", _config_string, "\"\"", "HALO_MENU_OPEN", _environment_value, _platform_all,
		"Start on this screen of the menus (port/assets/menus) instead of the main\n"
		"menu, a player profile being edited; empty for the main menu." },
	{ "debug.gpu_flush_draws", _config_integer, "-1", "HALO_GPU_FLUSH_DRAWS", _environment_value, _platform_desktop,
		"Flush the GPU's pipeline every this many draws: -1 for every 3 on Intel\n"
		"graphics with Mesa's driver (which can hang without), 0 never." },
	{ "debug.gpu_stats", _config_boolean, "false", "HALO_GPU_STATS", _environment_set_is_true, _platform_all,
		"Log the renderer's draw counts once a second." },
	{ "debug.gpu_trace_frame", _config_integer, "-1", "HALO_GPU_TRACE", _environment_value, _platform_all,
		"Log every draw of this frame; -1 none." },
	{ "debug.gpu_trace_constants", _config_boolean, "false", "HALO_GPU_TRACE_CONSTANTS", _environment_set_is_true, _platform_all,
		"With gpu_trace_frame, also the vertex shader constants." },
	{ "debug.gpu_skip_vertex_shaders", _config_string, "\"\"", "HALO_GPU_SKIP_VS", _environment_value, _platform_all,
		"Comma-separated ids of vertex shaders not to draw with." },
	{ "debug.gpu_dump_shaders", _config_string, "\"\"", "HALO_GPU_DUMP_SHADERS", _environment_value, _platform_all,
		"A folder to write the generated GLSL to; empty none." },
	{ "debug.gpu_debug_expression", _config_string, "\"\"", "HALO_GPU_DEBUG_EXPR", _environment_value, _platform_all,
		"A GLSL expression every pixel shader shows instead of its result." },
	{ "debug.gpu_debug_texture0", _config_boolean, "false", "HALO_GPU_DEBUG_T0", _environment_set_is_true, _platform_all,
		"Pixel shaders show their first texture." },
	{ "debug.gpu_debug_flat", _config_boolean, "false", "HALO_GPU_DEBUG_FLAT", _environment_set_is_true, _platform_all,
		"Pixel shaders show their vertex colour." },
	{ "debug.screenshot_directory", _config_string, "\"\"", "HALO_SCREENSHOT_DIR", _environment_value, _platform_all,
		"A folder to save frames to (with screenshot_every); empty none." },
	{ "debug.screenshot_every", _config_integer, "0", "HALO_SCREENSHOT_EVERY", _environment_value, _platform_all,
		"Save every this many frames to screenshot_directory; 0 none." },
	{ "debug.texture_dump_directory", _config_string, "\"\"", "HALO_TEXTURE_DUMP", _environment_value, _platform_all,
		"A folder to write every texture to as it is uploaded; empty none." },
	{ "debug.texture_log", _config_boolean, "false", "HALO_TEXTURE_LOG", _environment_set_is_true, _platform_all,
		"Log texture uploads." },
	{ "debug.texture_no_cache", _config_boolean, "false", "HALO_TEXTURE_NO_CACHE", _environment_set_is_true, _platform_all,
		"Upload textures again every time they are used." },
	{ "debug.sample_seconds", _config_real, "0.0", "HALO_SAMPLE", _environment_value, _platform_android,
		"Log where every game thread is this often, in seconds (read by the\n"
		"app, port/android/host/host_debug.c); 0 never." },
	{ "debug.memory_watch", _config_boolean, "true", NULL, _environment_value, _platform_android,
		"Notice the game's writes to cached textures and vertices by page\n"
		"protection; false compares page contents once a frame instead, which is\n"
		"slower. Under ARM translation (the x86 emulator) the app always compares\n"
		"contents. Read by the app from the file (port/android/host/host_main.c)." },
#ifdef HALO_PROFILE
	{ "debug.profile_record", _config_boolean, "false", "HALO_PROFILE_RECORD", _environment_value, _platform_all,
		"Record a profile with no command (configure.py --profile builds): from\n"
		"when profile_record_when says until profile_stop, a map change or the\n"
		"end, into numbered part files in the data folder's profiles folder\n"
		"(tools/net_report.py reads it)." },
	{ "debug.profile_record_when", _config_string, "\"start\"", "HALO_PROFILE_RECORD_WHEN", _environment_value,
		_platform_all,
		"\"start\": from the first frame until the first map change; \"game\":\n"
		"each game that is not the main menu, a recording each, until its map\n"
		"goes." },
	{ "debug.profile_memory", _config_integer, "256", "HALO_PROFILE_MEMORY", _environment_value, _platform_all,
		"Megabytes a recording keeps in memory, 4 to 1024: two halves, each\n"
		"written out as a part when it fills." },
#endif
};

#define NUMBER_OF_CONFIG_SETTINGS (sizeof(config_settings) / sizeof(config_settings[0]))

#ifdef HALO_ANDROID
#define CONFIG_PLATFORM _platform_android
#elif defined(HALO_WEB)
#define CONFIG_PLATFORM (_platform_desktop | _platform_web)
#elif defined(_WIN32)
#define CONFIG_PLATFORM (_platform_desktop | _platform_windows)
#else
#define CONFIG_PLATFORM _platform_desktop
#endif

struct config_value
{
	int boolean;
	long integer;
	double real;
	char *string;
};

static struct config_value config_values[NUMBER_OF_CONFIG_SETTINGS];
static int config_loaded = 0;
static pthread_mutex_t config_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---------- the file */

static void config_path(char *path, size_t size)
{
#if defined(HALO_ANDROID) || defined(HALO_WEB)
	/* the data folder, which the app names (port/android/host/host_main.c;
	on the web, port/web/src/web_main.c) */
	const char *root = getenv("HALO_DATA_ROOT");

	snprintf(path, size, "%s/config.toml", root && *root ? root : ".");
#else
	/* the executable's folder, with its separator */
	const char *base = SDL_GetBasePath();

	snprintf(path, size, "%sconfig.toml", base ? base : "");
#endif
}

/* the whole file, NUL terminated, or NULL; free() it */
static char *config_read_file(const char *path, size_t *size)
{
#ifdef HALO_ARM64_GUEST
	FILE *file = fopen(path, "rb");
	char *text = NULL;
	long length;

	if (!file)
		return NULL;
	if (fseek(file, 0, SEEK_END) == 0 && (length = ftell(file)) >= 0 && fseek(file, 0, SEEK_SET) == 0)
	{
		text = malloc((size_t)length + 1);
		if (text && fread(text, 1, (size_t)length, file) == (size_t)length)
		{
			text[length] = 0;
			*size = (size_t)length;
		}
		else
		{
			free(text);
			text = NULL;
		}
	}
	fclose(file);
	return text;
#else
	/* SDL's, for UTF-8 paths on Windows */
	void *data = SDL_LoadFile(path, size);
	char *text;

	if (!data)
		return NULL;
	text = malloc(*size + 1);
	if (text)
	{
		memcpy(text, data, *size);
		text[*size] = 0;
	}
	SDL_free(data);
	return text;
#endif
}

static int config_write_file(const char *path, const char *text)
{
#ifdef HALO_ARM64_GUEST
	FILE *file = fopen(path, "wb");
	int written;

	if (!file)
		return 0;
	written = fwrite(text, 1, strlen(text), file) == strlen(text);
	return fclose(file) == 0 && written;
#else
	return SDL_SaveFile(path, text, strlen(text));
#endif
}

struct config_text
{
	char *buffer;
	size_t length, capacity;
};

static void config_append(struct config_text *text, const char *string)
{
	size_t length = strlen(string);

	if (text->length + length + 1 > text->capacity)
	{
		size_t capacity = (text->capacity ? text->capacity : 4096) * 2 + length;
		char *buffer = realloc(text->buffer, capacity);

		if (!buffer)
			return;
		text->buffer = buffer;
		text->capacity = capacity;
	}
	memcpy(text->buffer + text->length, string, length + 1);
	text->length += length;
}

/* the first length characters of text, as a string of their own */
static char *config_copy(const char *text, size_t length)
{
	char *copy = malloc(length + 1);

	if (copy)
	{
		memcpy(copy, text, length);
		copy[length] = 0;
	}
	return copy;
}

/* one setting as the file holds it: its comment, and its key at the
default */
static void config_append_setting(struct config_text *text, const struct config_setting *setting)
{
	const char *dot = strchr(setting->name, '.');
	const char *line;
	char buffer[256];

	config_append(text, "\n");
	for (line = setting->comment; *line;)
	{
		size_t length = strcspn(line, "\n");

		snprintf(buffer, sizeof(buffer), "# %.*s\n", (int)length, line);
		config_append(text, buffer);
		line += length;
		if (*line)
			line++;
	}
#ifndef HALO_ANDROID
	/* (Android apps have no environment to set) */
	switch (setting->environment_style)
	{
	case _environment_value:
		snprintf(buffer, sizeof(buffer), "# (for one run: %s=<value>)\n", setting->environment);
		break;
	case _environment_set_is_true:
		snprintf(buffer, sizeof(buffer), "# (for one run: %s=1 makes it true)\n", setting->environment);
		break;
	case _environment_set_is_false:
		snprintf(buffer, sizeof(buffer), "# (for one run: %s=1 makes it false)\n", setting->environment);
		break;
	}
	config_append(text, buffer);
#endif
	snprintf(buffer, sizeof(buffer), "%s = %s\n", dot + 1, setting->default_value);
	config_append(text, buffer);
}

/* the file with every setting of this build at its default */
static char *config_default_text(void)
{
	struct config_text text = { NULL, 0, 0 };
	char section[32] = "";
	size_t index;

#ifdef HALO_ANDROID
	config_append(&text,
		"# Halo settings\n"
		"#\n"
		"# The game writes this file with the defaults when it is missing: delete\n"
		"# it to go back to them.\n");
#else
	config_append(&text,
		"# Halo settings\n"
		"#\n"
		"# The game writes this file with the defaults when it is missing: delete\n"
		"# it to go back to them. Each setting can also be set for one run with\n"
		"# the environment variable named with it, which wins over this file.\n");
#endif
	for (index = 0; index < NUMBER_OF_CONFIG_SETTINGS; index++)
	{
		const struct config_setting *setting = &config_settings[index];
		const char *dot = strchr(setting->name, '.');
		char buffer[64];

		if (!(setting->platforms & CONFIG_PLATFORM) || !dot)
			continue;
		if (strncmp(section, setting->name, (size_t)(dot - setting->name)) ||
			section[dot - setting->name] != 0)
		{
			snprintf(section, sizeof(section), "%.*s", (int)(dot - setting->name), setting->name);
			snprintf(buffer, sizeof(buffer), "\n[%s]\n", section);
			config_append(&text, buffer);
		}
		config_append_setting(&text, setting);
	}
	return text.buffer;
}

/* the settings of this build that text (the file, parsed as table) lacks,
added to it in their sections, keeping the rest as it is: a newer version's
settings appear in an older file. Returns the new text, or NULL if nothing
was missing */
static char *config_add_missing(const char *text, toml_datum_t table)
{
	char *result = NULL;
	size_t index;

	for (index = 0; index < NUMBER_OF_CONFIG_SETTINGS; index++)
	{
		const struct config_setting *setting = &config_settings[index];
		const char *dot = strchr(setting->name, '.');
		const char *current = result ? result : text;
		struct config_text block = { NULL, 0, 0 };
		struct config_text updated = { NULL, 0, 0 };
		char header[40];
		const char *line;
		const char *insert = NULL;

		if (!(setting->platforms & CONFIG_PLATFORM) || !dot || toml_seek(table, setting->name).type != TOML_UNKNOWN)
			continue;
		snprintf(header, sizeof(header), "[%.*s]", (int)(dot - setting->name), setting->name);
		/* the end of the section's last line that is not blank */
		for (line = current; *line; )
		{
			const char *start = line;
			size_t length = strcspn(line, "\n");

			while (*start == ' ' || *start == '\t')
				start++;
			if (insert && *start == '[')
				break;
			if (!insert && !strncmp(start, header, strlen(header)))
				insert = line + length;
			else if (insert && start < line + length && *start != '\r')
				insert = line + length;
			line += length;
			if (*line)
				line++;
		}
		if (insert)
		{
			if (*insert)
				insert++;
			config_append_setting(&block, setting);
		}
		else
		{
			/* no such section: a new one at the end */
			insert = current + strlen(current);
			config_append(&block, current[0] && insert[-1] != '\n' ? "\n\n" : "\n");
			config_append(&block, header);
			config_append(&block, "\n");
			config_append_setting(&block, setting);
		}
		if (!block.buffer)
			continue;
		{
			char *before = config_copy(current, (size_t)(insert - current));

			if (before)
				config_append(&updated, before);
			free(before);
		}
		if (insert > current && insert[-1] != '\n')
			config_append(&updated, "\n");
		config_append(&updated, block.buffer);
		config_append(&updated, insert);
		free(block.buffer);
		if (updated.buffer)
		{
			free(result);
			result = updated.buffer;
			platform_log("settings: added %s (new in this version) at its default", setting->name);
		}
	}
	return result;
}

/* ---------- values */

static int config_text_is_false(const char *text)
{
	char lower[8];
	size_t index;

	for (index = 0; index + 1 < sizeof(lower) && text[index]; index++)
		lower[index] = (char)tolower((unsigned char)text[index]);
	lower[index] = 0;
	return !strcmp(lower, "0") || !strcmp(lower, "false") || !strcmp(lower, "no") || !strcmp(lower, "off");
}

/* whether a variable that only has to be set (_environment_set_is_true or
_environment_set_is_false) is: empty, "0", "false", "no" or "off" is not */
static int config_environment_set(const char *text)
{
	return text[0] && !config_text_is_false(text);
}

static void config_set_from_text(struct config_value *value, enum config_type type, const char *text)
{
	switch (type)
	{
	case _config_boolean:
		value->boolean = !config_text_is_false(text);
		break;
	case _config_integer:
		value->integer = strtol(text, NULL, 10);
		break;
	case _config_real:
		value->real = strtod(text, NULL);
		break;
	case _config_string:
		/* (the old string is kept, not freed: config_string's callers hold
		its pointer, and Settings writes few, seldom) */
		value->string = strdup(text);
		break;
	}
}

/* the value in the file, if it is there and of the setting's type */
static void config_set_from_file(struct config_value *value, const struct config_setting *setting,
	toml_datum_t table)
{
	toml_datum_t datum = toml_seek(table, setting->name);
	int wrong_type = 0;

	if (datum.type == TOML_UNKNOWN)
		return;
	switch (setting->type)
	{
	case _config_boolean:
		if (datum.type == TOML_BOOLEAN)
			value->boolean = datum.u.boolean;
		else
			wrong_type = 1;
		break;
	case _config_integer:
		if (datum.type == TOML_INT64)
			value->integer = (long)datum.u.int64;
		else
			wrong_type = 1;
		break;
	case _config_real:
		if (datum.type == TOML_FP64)
			value->real = datum.u.fp64;
		else if (datum.type == TOML_INT64)
			value->real = (double)datum.u.int64;
		else
			wrong_type = 1;
		break;
	case _config_string:
		if (datum.type == TOML_STRING)
		{
			free(value->string);
			value->string = strdup(datum.u.s);
		}
		else
		{
			wrong_type = 1;
		}
		break;
	}
	if (wrong_type)
	{
		static const char *const expected[] = { "true or false", "a whole number", "a number", "a quoted string" };

		platform_log("config.toml line %d: %s should be %s; using %s", datum.lineno, setting->name,
			expected[setting->type], setting->default_value);
	}
}

/* how many times a setting has been written (config_write): readers that
keep a setting watch this, to read it again */
static volatile unsigned long config_change_count;

static long config_setting_index(const char *name)
{
	size_t index;

	for (index = 0; index < NUMBER_OF_CONFIG_SETTINGS; index++)
	{
		if (!strcmp(config_settings[index].name, name))
			return (long)index;
	}
	return -1;
}

/* keys in the file that are no setting, likely misspelt */
static void config_report_unknown_keys(toml_datum_t table)
{
	int section_index;

	for (section_index = 0; section_index < table.u.tab.size; section_index++)
	{
		toml_datum_t section = table.u.tab.value[section_index];
		int key_index;

		if (section.type != TOML_TABLE)
		{
			platform_log("config.toml line %d: unknown setting %s", section.lineno, table.u.tab.key[section_index]);
			continue;
		}
		for (key_index = 0; key_index < section.u.tab.size; key_index++)
		{
			char name[128];

			snprintf(name, sizeof(name), "%s.%s", table.u.tab.key[section_index], section.u.tab.key[key_index]);
			if (config_setting_index(name) < 0)
				platform_log("config.toml line %d: unknown setting %s", section.u.tab.value[key_index].lineno, name);
		}
	}
}

static void config_load(void)
{
	char path[1024];
	size_t size = 0;
	char *text;
	size_t index;

	for (index = 0; index < NUMBER_OF_CONFIG_SETTINGS; index++)
	{
		const char *default_value = config_settings[index].default_value;

		if (config_settings[index].type == _config_string)
		{
			/* written as a TOML basic string without escapes */
			size_t length = strlen(default_value);

			config_values[index].string = length >= 2 ? config_copy(default_value + 1, length - 2) : strdup("");
		}
		else
		{
			config_set_from_text(&config_values[index], config_settings[index].type, default_value);
		}
	}

	config_path(path, sizeof(path));
	text = config_read_file(path, &size);
	if (text)
	{
		toml_result_t result = toml_parse(text, (int)size);

		if (result.ok)
		{
			char *completed;

			for (index = 0; index < NUMBER_OF_CONFIG_SETTINGS; index++)
				config_set_from_file(&config_values[index], &config_settings[index], result.toptab);
			config_report_unknown_keys(result.toptab);
			platform_log("settings: %s", path);
			completed = config_add_missing(text, result.toptab);
			if (completed && !config_write_file(path, completed))
				platform_log("settings: cannot write %s", path);
			free(completed);
		}
		else
		{
			platform_log("config.toml: %s; using the defaults", result.errmsg);
		}
		toml_free(result);
		free(text);
	}
	else
	{
		char *defaults = config_default_text();

		if (defaults && config_write_file(path, defaults))
			platform_log("settings: wrote the defaults to %s", path);
		else
			platform_log("settings: cannot write %s; using the defaults", path);
		free(defaults);
	}

	for (index = 0; index < NUMBER_OF_CONFIG_SETTINGS; index++)
	{
		const struct config_setting *setting = &config_settings[index];
		const char *environment = setting->environment ? getenv(setting->environment) : NULL;

		if (!environment || (setting->environment_style != _environment_value && !config_environment_set(environment)))
			continue;
		switch (setting->environment_style)
		{
		case _environment_value:
			config_set_from_text(&config_values[index], setting->type, environment);
			break;
		case _environment_set_is_true:
			config_values[index].boolean = 1;
			break;
		case _environment_set_is_false:
			config_values[index].boolean = 0;
			break;
		}
	}
}

static const struct config_value *config_value(const char *name, enum config_type type)
{
	static const struct config_value none = { 0, 0, 0.0, "" };
	long index;

	pthread_mutex_lock(&config_lock);
	if (!config_loaded)
	{
		config_load();
		config_loaded = 1;
	}
	pthread_mutex_unlock(&config_lock);
	index = config_setting_index(name);
	if (index < 0 || config_settings[index].type != type)
	{
		platform_log("settings: no %s setting %s", type == _config_string ? "string" : "such", name);
		return &none;
	}
	return &config_values[index];
}

/* ---------- writing a setting */

/* the line's key, if it is "key = ..." (after spaces), in key */
static int config_line_key(const char *line, const char *end, const char *key)
{
	size_t length = strlen(key);

	while (line < end && (*line == ' ' || *line == '\t'))
		line++;
	if ((size_t)(end - line) <= length || strncmp(line, key, length) != 0)
		return 0;
	line += length;
	while (line < end && (*line == ' ' || *line == '\t'))
		line++;
	return line < end && *line == '=';
}

/* the section the line opens, if it is "[section]" (after spaces) */
static int config_line_section(const char *line, const char *end, char *section, size_t size)
{
	const char *close;

	while (line < end && (*line == ' ' || *line == '\t'))
		line++;
	if (line >= end || *line != '[')
		return 0;
	close = memchr(line, ']', (size_t)(end - line));
	if (!close || (size_t)(close - line - 1) >= size)
		return 0;
	memcpy(section, line + 1, (size_t)(close - line - 1));
	section[close - line - 1] = 0;
	return 1;
}

/* sets a setting, for now and in config.toml, from its value as text
("true", "60", "1.5", "all"): its line there is changed (or added), the rest
of the file kept as it is */
int config_write(const char *name, const char *value)
{
	const char *dot = strchr(name, '.');
	long index = config_setting_index(name);
	char section[64], key[64], wanted[80], current[64] = "", line_text[600], path[1024];
	struct config_text out = { 0 };
	size_t size = 0;
	char *text;
	const char *line;
	int written = 0, in_section = 0, succeeded;

	if (index < 0 || !dot || (size_t)(dot - name) >= sizeof(section) || strlen(value) > 256)
		return 0;
	/* (the file read first, as the other settings are) */
	config_value(name, config_settings[index].type);
	pthread_mutex_lock(&config_lock);
	config_set_from_text(&config_values[index], config_settings[index].type, value);
	snprintf(section, sizeof(section), "%.*s", (int)(dot - name), name);
	snprintf(key, sizeof(key), "%s", dot + 1);
	switch (config_settings[index].type)
	{
	case _config_boolean:
		snprintf(line_text, sizeof(line_text), "%s = %s\n", key, config_values[index].boolean ? "true" : "false");
		break;
	case _config_integer:
		snprintf(line_text, sizeof(line_text), "%s = %ld\n", key, config_values[index].integer);
		break;
	case _config_real:
		/* (with its point: TOML reads 1 as an integer) */
		snprintf(line_text, sizeof(line_text), "%s = %.15g", key, config_values[index].real);
		if (!strpbrk(line_text + strlen(key) + 3, ".en"))
			strcat(line_text, ".0");
		strcat(line_text, "\n");
		break;
	case _config_string:
	{
		char *end = line_text + snprintf(line_text, sizeof(line_text), "%s = \"", key);
		const char *character;

		for (character = config_values[index].string; *character; character++)
		{
			if (*character == '"' || *character == '\\')
				*end++ = '\\';
			*end++ = *character;
		}
		strcpy(end, "\"\n");
		break;
	}
	}
	snprintf(wanted, sizeof(wanted), "%s", section);
	config_path(path, sizeof(path));
	text = config_read_file(path, &size);
	for (line = text ? text : ""; *line;)
	{
		const char *end = line + strcspn(line, "\n");
		const char *next = *end ? end + 1 : end;

		if (config_line_section(line, end, current, sizeof(current)))
		{
			/* (leaving the section without the key: it goes at its end) */
			if (in_section && !written)
			{
				config_append(&out, line_text);
				written = 1;
			}
			in_section = !strcmp(current, wanted);
		}
		else if (in_section && !written && config_line_key(line, end, key))
		{
			config_append(&out, line_text);
			written = 1;
			line = next;
			continue;
		}
		{
			char *copy = config_copy(line, (size_t)(next - line));

			if (copy)
			{
				config_append(&out, copy);
				free(copy);
			}
		}
		line = next;
	}
	if (!written)
	{
		if (out.length && out.buffer[out.length - 1] != '\n')
			config_append(&out, "\n");
		if (!in_section)
		{
			char header[80];

			snprintf(header, sizeof(header), "\n[%s]\n", section);
			config_append(&out, header);
		}
		config_append(&out, line_text);
	}
	succeeded = out.buffer && config_write_file(path, out.buffer);
	config_change_count++;
	pthread_mutex_unlock(&config_lock);
	free(out.buffer);
	free(text);
	return succeeded;
}

int config_write_boolean(const char *name, int value)
{
	long index = config_setting_index(name);

	return index >= 0 && config_settings[index].type == _config_boolean && config_write(name, value ? "true" : "false");
}

/* a setting's value as text ("true", "60", "1.5", "all"): 0 if there is no
such setting */
int config_text(const char *name, char *text, size_t size)
{
	long index = config_setting_index(name);
	const struct config_value *value;

	if (index < 0)
		return 0;
	value = config_value(name, config_settings[index].type);
	switch (config_settings[index].type)
	{
	case _config_boolean:
		snprintf(text, size, "%s", value->boolean ? "true" : "false");
		break;
	case _config_integer:
		snprintf(text, size, "%ld", value->integer);
		break;
	case _config_real:
		snprintf(text, size, "%.15g", value->real);
		break;
	case _config_string:
		snprintf(text, size, "%s", value->string ? value->string : "");
		break;
	}
	return 1;
}

void config_folder(char *path, size_t size)
{
	char file[1024];
	char *separator;

	config_path(file, sizeof(file));
	separator = strrchr(file, '/');
#ifndef HALO_ANDROID
	if (!separator || (strrchr(file, '\\') && strrchr(file, '\\') > separator))
		separator = strrchr(file, '\\');
#endif
	if (separator)
		separator[1] = 0;
	else
		file[0] = 0;
	snprintf(path, size, "%s", file);
}

/* ---------- public code */

char *config_file_read(const char *path, size_t *size)
{
	return config_read_file(path, size);
}

unsigned long config_changes(void)
{
	return config_change_count;
}

int config_default(const char *name, char *text, size_t size)
{
	long index = config_setting_index(name);
	const char *value;
	size_t length;

	if (index < 0)
		return 0;
	value = config_settings[index].default_value;
	length = strlen(value);
	/* (a string's without its quotes: the defaults have no escapes) */
	if (config_settings[index].type == _config_string && length >= 2 && value[0] == '"')
	{
		value++;
		length -= 2;
	}
	snprintf(text, size, "%.*s", (int)length, value);
	return 1;
}

int config_boolean(const char *name)
{
	return config_value(name, _config_boolean)->boolean;
}

long config_integer(const char *name)
{
	return config_value(name, _config_integer)->integer;
}

double config_real(const char *name)
{
	return config_value(name, _config_real)->real;
}

const char *config_string(const char *name)
{
	const char *string = config_value(name, _config_string)->string;

	return string ? string : "";
}
