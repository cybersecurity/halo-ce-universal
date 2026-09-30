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
	/* the variable being set at all makes it true */
	_environment_set_is_true,
	/* the variable being set at all makes it false */
	_environment_set_is_false,
};

/* the builds a setting means something in, and is written for */
enum
{
	_platform_desktop = 1,
	_platform_android = 2,
	_platform_all = _platform_desktop | _platform_android,
};

struct config_setting
{
	const char *name;
	enum config_type type;
	/* as it is written in the file */
	const char *default_value;
	const char *environment;
	enum config_environment environment_style;
	unsigned platforms;
	const char *comment;
};

/* the ray-traced lighting's default: on for the macOS port, which it was
made for; off elsewhere */
#ifdef HALO_MACOS
#define HALO_CONFIG_RAY_TRACING "\"on\""
#else
#define HALO_CONFIG_RAY_TRACING "\"off\""
#endif

/* the resolution: 1080p on macOS (Retina displays' native pixels are many,
and the ray-traced lighting's cost follows them); the display's elsewhere */
#ifdef HALO_MACOS
#define HALO_CONFIG_RESOLUTION "\"1080p\""
#else
#define HALO_CONFIG_RESOLUTION "\"native\""
#endif

/* a default the macOS port turns off */
#ifdef HALO_MACOS
#define HALO_CONFIG_MACOS_FALSE "false"
#else
#define HALO_CONFIG_MACOS_FALSE "true"
#endif

static const struct config_setting config_settings[] =
{
	{ "display.fullscreen", _config_boolean, "true", "HALO_FULLSCREEN", _environment_value, _platform_desktop,
		"Start fullscreen, in the display's shape; false starts in a window, in\n"
		"the window's shape. display.resolution sets the pixels. F11 switches." },
	{ "display.resolution", _config_string, HALO_CONFIG_RESOLUTION, "HALO_RESOLUTION", _environment_value, _platform_desktop,
		"The picture's pixels: \"native\" (the display's in fullscreen, the window's\n"
		"in a window), \"720p\", \"1080p\", \"1440p\", \"2160p\", \"<width>x<height>\",\n"
		"or \"xbox\" for the Xbox's 640x480. F8 steps through them while playing." },
	{ "display.render_scale", _config_real, "1.0", "HALO_RENDER_SCALE", _environment_value, _platform_desktop,
		"Multiplies the resolution: below 1.0 draws fewer pixels (faster),\n"
		"above 1.0 more (supersampling, smoother edges); up to 4.0." },
	{ "display.window_scale", _config_integer, "2", "HALO_WINDOW_SCALE", _environment_value, _platform_desktop,
		"The window's size as a multiple of 640x480 (it can be resized)." },
	{ "display.screen_width", _config_integer, "0", "HALO_SCREEN_WIDTH", _environment_value, _platform_android,
		"Columns of the 480-line picture: 0 for the display's shape, 640 for the\n"
		"Xbox's 4:3." },
	{ "display.vsync", _config_boolean, "true", "HALO_NO_VSYNC", _environment_set_is_false, _platform_all,
		"Wait for the display between frames; false draws as fast as possible." },
	{ "display.interpolation", _config_boolean, "true", "HALO_INTERPOLATION", _environment_value, _platform_all,
		"Draw a frame for every display refresh, blending between the game's 30\n"
		"ticks a second; false keeps the original 30 frames a second." },
	{ "display.direct_camera", _config_boolean, "true", "HALO_DIRECT_CAMERA", _environment_value, _platform_desktop,
		"In first person, point the view where the player aims now instead of\n"
		"where the last tick left it: the view turns the frame the mouse moves,\n"
		"not up to two ticks (66 ms) later." },

	{ "display.show_fps", _config_boolean, "false", "HALO_SHOW_FPS", _environment_value, _platform_desktop,
		"Show the game's frames-a-second counter (F7, or Command-P on a Mac,\n"
		"shows or hides it)." },

	{ "display.ray_tracing", _config_string, HALO_CONFIG_RAY_TRACING, "HALO_RAY_TRACING", _environment_value,
		_platform_all,
		"Ray-traced lighting on the 3D world (port/linux/src/raytrace_gl.c): \"on\"\n"
		"(on macOS with Metal's rays through the level), \"screen\" (the screen's rays\n"
		"only), \"off\", or \"occlusion\" and \"depth\" to see what it works from, or\n"
		"\"rays\" and \"split\" to see what Metal's rays find. F9 switches it while\n"
		"playing; F6 steps through the views." },
	{ "display.ray_tracing_occlusion", _config_real, "0.8", "HALO_RAY_TRACING_OCCLUSION", _environment_value,
		_platform_all,
		"How much traced ambient occlusion darkens creases and corners, 0.0 to 1.0." },
	{ "display.ray_tracing_reflections", _config_real, "0.25", "HALO_RAY_TRACING_REFLECTIONS", _environment_value,
		_platform_all,
		"How strongly surfaces reflect the traced scene, 0.0 to 1.0." },
	{ "display.ray_tracing_bounce", _config_real, "0.25", "HALO_RAY_TRACING_BOUNCE", _environment_value,
		_platform_all,
		"How much light one traced bounce carries between surfaces, 0.0 to 1.0." },
	{ "display.ray_tracing_shadows", _config_real, "1.0", "HALO_RAY_TRACING_SHADOWS", _environment_value,
		_platform_all,
		"How dark the sun's traced shadows on characters, vehicles and items are,\n"
		"0.0 to 1.0 (macOS, with Metal's rays)." },
	{ "display.ray_tracing_shapes", _config_string, "\"model\"", "HALO_RAY_TRACING_SHAPES", _environment_value,
		_platform_all,
		"The characters' and vehicles' shapes in Metal's rays: \"model\" (their drawn\n"
		"models, skinned as drawn) or \"collision\" (the meshes their bullets hit);\n"
		"F4 switches them (macOS)." },
	{ "display.ray_tracing_lights", _config_string, "\"traced\"", "HALO_RAY_TRACING_LIGHTS", _environment_value,
		_platform_all,
		"The lights on the level in Metal's rays: \"traced\" (every light the game has,\n"
		"its own lights too - Guilty Spark's, the glows' - traced in its colour, with\n"
		"its shadows, in place of the game's) or \"game\" (the game's dynamic lights as\n"
		"it draws them, only their shadows traced) (macOS)." },
	{ "display.ray_tracing_level", _config_string, "\"render\"", "HALO_RAY_TRACING_LEVEL", _environment_value,
		_platform_all,
		"The level in Metal's rays: \"render\" (its drawn triangles, as you see them)\n"
		"or \"collision\" (the surfaces the game collides with) (macOS)." },
	{ "display.ray_tracing_gi", _config_string, "\"traced\"", "HALO_RAY_TRACING_GI", _environment_value,
		_platform_all,
		"The level's light traced in place of its lightmaps (macOS): \"traced\" (the\n"
		"sun, the sky, the glowing surfaces and the lights, with the lightmaps' light\n"
		"where the rays land as the light that bounced), \"black\" (without the\n"
		"lightmaps at all: only what the rays find lit), \"path\" (without the lightmaps,\n"
		"traced over 3 bounces: the sun, the sky and the glowing surfaces lighting\n"
		"each place the light bounces from; the slowest) or \"off\"." },
	{ "display.ray_tracing_gi_sun", _config_real, "1.0", "HALO_RAY_TRACING_GI_SUN", _environment_value,
		_platform_all, "The traced sun's strength (display.ray_tracing_gi)." },
	{ "display.ray_tracing_gi_bounce", _config_real, "1.0", "HALO_RAY_TRACING_GI_BOUNCE", _environment_value,
		_platform_all, "The traced bounced light's strength (display.ray_tracing_gi)." },
	{ "display.ray_tracing_gi_glow", _config_real, "1.0", "HALO_RAY_TRACING_GI_GLOW", _environment_value,
		_platform_all, "The glowing surfaces' traced light's strength (display.ray_tracing_gi)." },
	{ "display.ray_tracing_bounces", _config_real, "3", "HALO_RAY_TRACING_BOUNCES", _environment_value,
		_platform_all,
		"How many times the path tracer's light bounces at most, 1 to 4 (display.ray_tracing_gi\n"
		"\"path\"): more reach farther into the shade, and cost more. The governor takes one\n"
		"off under load, and all but one when the frames are slow." },
	{ "display.ray_tracing_samples", _config_real, "1", "HALO_RAY_TRACING_SAMPLES", _environment_value,
		_platform_all,
		"The traced light's rays a pixel each time it takes new ones, 1 to 8: more, less\n"
		"noise and blotches, and that many times the cost of its rays." },
	{ "display.ray_tracing_gi_split", _config_boolean, "false", "HALO_RAY_TRACING_GI_SPLIT", _environment_set_is_true,
		_platform_all, "The game's light on the screen's left half, the traced on its right (display.ray_tracing_gi)." },
	{ "display.ray_tracing_gi_lights", _config_real, "0.5", "HALO_RAY_TRACING_GI_LIGHTS", _environment_value,
		_platform_all, "The traced lights' and glows' strength in the light buffer (display.ray_tracing_gi)." },
	{ "display.ray_tracing_objects", _config_boolean, "true", "HALO_RAY_TRACING_OBJECTS", _environment_value,
		_platform_all,
		"The characters and vehicles in Metal's rays too: their contact shadows on\n"
		"the level, and your own body's shadow (macOS)." },

	{ "audio.enabled", _config_boolean, "true", "HALO_NO_AUDIO", _environment_set_is_false, _platform_all,
		"Play sound." },
	{ "audio.volume", _config_real, "1.0", "HALO_VOLUME", _environment_value, _platform_all,
		"The volume of everything, 0.0 to 1.0." },

	{ "input.mouse_sensitivity", _config_real, "1.0", "HALO_MOUSE_SENSITIVITY", _environment_value, _platform_desktop,
		"How far the view turns for the mouse's movement." },
	{ "input.invert_mouse", _config_boolean, "false", "HALO_MOUSE_INVERT", _environment_set_is_true, _platform_desktop,
		"Moving the mouse forward looks down." },
	{ "input.mouse_aim_assist", _config_boolean, "false", "HALO_MOUSE_AIM_ASSIST", _environment_value, _platform_desktop,
		"Magnetism while aiming with the mouse, as with a controller: the view\n"
		"slowed and dragged along by a target. The last of the mouse and the\n"
		"right stick to move decides. The bullets' autoaim (bent toward the\n"
		"target) stays either way." },

	{ "game.map", _config_string, "\"\"", "HALO_MAP", _environment_value, _platform_all,
		"A map to start at start-up, after init.txt: a campaign level's name\n"
		"(\"a10\", \"b30\"), a multiplayer map's (\"bloodgulch\"), or a scenario\n"
		"path (\"levels\\\\b30\\\\b30\"); empty for the menu." },
	{ "game.language", _config_string, "\"\"", "HALO_LANGUAGE", _environment_value, _platform_all,
		"The language the game asks the Xbox for: \"ja\", \"de\", \"fr\", \"es\" or \"it\";\n"
		"empty for English. The game data decides what is translated." },

	{ "paths.data", _config_string, "\"\"", "HALO_DATA_ROOT", _environment_value, _platform_desktop,
		"The folder holding the game data's maps folder; empty looks in the\n"
		"working directory and its assets folder. Windows paths are easiest in\n"
		"single quotes: 'C:\\Games\\Halo'." },
	{ "paths.saves", _config_string, "\"\"", "HALO_SAVE_ROOT", _environment_value, _platform_desktop,
		"Where saved games and profiles go; empty for the usual place\n"
		"(~/.local/share/halo-linux, %APPDATA%\\halo on Windows,\n"
		"~/Library/Application Support/Halo on macOS)." },

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
	/* off on macOS: a link that happens to be on the clipboard should not
	join a stranger's game (port/macos/README.md) */
	{ "network.join_from_clipboard", _config_boolean, HALO_CONFIG_MACOS_FALSE, "HALO_NET_JOIN_FROM_CLIPBOARD",
		_environment_value,
		_platform_all,
		"Join the game of an invite link found on the clipboard when the game\n"
		"comes to the front." },
	{ "network.tunnel_port", _config_integer, "0", "HALO_NET_TUNNEL_PORT", _environment_value, _platform_all,
		"The UDP port internet play uses; 0 picks one. A fixed one can be\n"
		"forwarded on the router, for networks whose NAT stops connections." },
	/* off on macOS: the local network needs no forwarded port */
	{ "network.allow_upnp", _config_boolean, HALO_CONFIG_MACOS_FALSE, "HALO_NET_ALLOW_UPNP", _environment_value,
		_platform_all,
		"Let internet play ask the router (UPnP) to forward its port, for\n"
		"networks whose NAT stops connections: when a player joins this\n"
		"machine's game, and when joining a game takes too long. False never\n"
		"asks." },
	{ "network.signalling_brokers", _config_string,
		"\"broker.emqx.io:1883,broker.hivemq.com:1883,test.mosquitto.org:1883\"",
		"HALO_NET_BROKERS", _environment_value, _platform_all,
		"Public MQTT brokers through which the machines of an invite find each\n"
		"other (its messages are encrypted); comma-separated host:port." },
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

	{ "debug.network_test", _config_string, "\"\"", "HALO_NETWORK_TEST", _environment_value, _platform_all,
		"Automated system link sessions for testing (port/linux/game/network_test.c):\n"
		"\"host:<map>\" hosts a game on that map, \"join\" joins the first game found;\n"
		"empty for none." },
	{ "debug.network_test_start", _config_real, "15.0", "HALO_NETWORK_TEST_START", _environment_value, _platform_all,
		"Seconds after hosting that an automated test game starts." },
	{ "debug.network_test_kill", _config_real, "0.0", "HALO_NETWORK_TEST_KILL", _environment_value, _platform_all,
		"Every this many seconds an automated test host kills its last player; 0 never." },
	{ "debug.network_test_score", _config_integer, "0", "HALO_NETWORK_TEST_SCORE", _environment_value, _platform_all,
		"The score an automated test host's game type plays to (a short game, to\n"
		"test the next); 0 the game type's own." },
	{ "debug.network_test_shoot", _config_real, "0.0", "HALO_NETWORK_TEST_SHOOT", _environment_value, _platform_all,
		"Every this many seconds each automated test player hits the next with\n"
		"their weapon; 0 never." },
	{ "debug.network_test_vehicle", _config_real, "0.0", "HALO_NETWORK_TEST_VEHICLE", _environment_value, _platform_all,
		"This many seconds into an automated test game the host seats its last\n"
		"player as a vehicle's driver (and out 15 seconds on); 0 never." },
	{ "debug.network_test_pickup", _config_real, "0.0", "HALO_NETWORK_TEST_PICKUP", _environment_value, _platform_all,
		"This many seconds into an automated test game the host stands its last\n"
		"player on a weapon, which a joining player then picks up; 0 never." },
	{ "debug.telnet_console", _config_boolean, "false", "HALO_TELNET_CONSOLE", _environment_set_is_true, _platform_all,
		"Listen on 127.0.0.1 port 23 (telnet) for a script console that runs what\n"
		"it is sent as the game's console does, with no password; false none." },
	{ "debug.network_latency", _config_real, "0.0", "HALO_NETWORK_LATENCY", _environment_value, _platform_all,
		"Milliseconds everything received is held back (a round trip between two\n"
		"machines of twice it), to test the netcode as over the internet; 0 none." },
	{ "debug.network_loss", _config_real, "0.0", "HALO_NETWORK_LOSS", _environment_value, _platform_all,
		"Percent of datagrams received that are dropped, for the same; 0 none." },
	{ "debug.commands", _config_string, "\"\"", "HALO_COMMANDS", _environment_value, _platform_all,
		"Console commands at times, for tests: \"<seconds>=<command>;...\" (seconds\n"
		"since the game started), such as \"47=cheat_all_weapons;50=cheat_spawn_warthog\";\n"
		"empty for none." },
	{ "debug.test_input", _config_string, "\"\"", "HALO_TEST_INPUT", _environment_value, _platform_all,
		"\"bot:<seed>\" plays controller 1 with a scripted pattern (automated\n"
		"network tests); \"look:<seed>\" stands still, only turning and looking\n"
		"up and down; \"script:<from>-<to>=<action>,...\" plays the actions in\n"
		"those seconds (forward, back, left, right, turnleft, turnright, up, down,\n"
		"fire, grenade, jump, crouch, zoom, action, flashlight, reload, switch, start);\n"
		"empty for none." },
	{ "debug.settings_script", _config_string, "\"\"", "HALO_SETTINGS_SCRIPT", _environment_value, _platform_desktop,
		"Keys for the settings overlay (F10) at times, for tests with screenshots:\n"
		"\"<seconds>=<key>,<key>...;...\" (seconds since the game started); the keys:\n"
		"open, close, up, down, left, right, enter, escape, pageup, pagedown, home,\n"
		"end, or move:<x>:<y>, click:<x>:<y>, wheel:<steps> (x and y: 0 to 1 of the\n"
		"window); empty for none." },
	{ "debug.update_answer", _config_string, "\"\"", "HALO_UPDATE_ANSWER", _environment_value, _platform_desktop,
		"The answer to the new version question, for automated tests: \"yes\",\n"
		"\"no\" or \"never\" (do not ask again, confirmed); empty asks." },
	{ "debug.exit_after", _config_real, "0.0", "HALO_EXIT_AFTER", _environment_value, _platform_all,
		"Quit this many seconds after the window opens; 0 never." },
	{ "debug.hidden_window", _config_boolean, "false", "HALO_HIDDEN_WINDOW", _environment_set_is_true, _platform_desktop,
		"Keep the window hidden (and never fullscreen)." },
	{ "debug.null_renderer", _config_boolean, "false", "HALO_NULL_RENDERER", _environment_set_is_true, _platform_all,
		"Run without a window, drawing nothing." },
	{ "debug.gl_debug", _config_boolean, "false", "HALO_GL_DEBUG", _environment_set_is_true, _platform_all,
		"Report OpenGL errors in the log." },
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
};

#define NUMBER_OF_CONFIG_SETTINGS (sizeof(config_settings) / sizeof(config_settings[0]))

#ifdef HALO_ANDROID
#define CONFIG_PLATFORM _platform_android
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
#ifdef HALO_ANDROID
	/* the data folder, which the app names (port/android/host/host_main.c) */
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
#ifdef HALO_ANDROID
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
#ifdef HALO_ANDROID
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
		free(value->string);
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

/* the command line's short names for settings */
static const struct
{
	const char *alias;
	const char *name;
	const char *value;
} config_aliases[] = {
	{ "gi", "display.ray_tracing_gi", NULL },
	{ "rt", "display.ray_tracing", NULL },
	{ "map", "game.map", NULL },
	{ "fps", "display.show_fps", NULL },
	{ "windowed", "display.fullscreen", "false" },
	{ "no_vsync", "display.vsync", "false" },
	{ "mute", "audio.enabled", "false" },
};

/* a setting by the command line's name for it: its whole name, an alias,
or the last part of one name alone ("vsync": display.vsync); -1 if none */
static long config_argument_index(const char *name, const char **value)
{
	size_t index, length = strlen(name);
	long found = -1;

	for (index = 0; index < sizeof(config_aliases) / sizeof(config_aliases[0]); index++)
	{
		if (!strcmp(name, config_aliases[index].alias))
		{
			if (config_aliases[index].value)
				*value = config_aliases[index].value;
			return config_setting_index(config_aliases[index].name);
		}
	}
	if ((found = config_setting_index(name)) >= 0)
		return found;
	for (index = 0; index < NUMBER_OF_CONFIG_SETTINGS; index++)
	{
		const char *setting = config_settings[index].name, *dot = strrchr(setting, '.');

		if (dot && strlen(dot + 1) == length && !strcmp(dot + 1, name))
		{
			if (found >= 0)
				return -2;
			found = (long)index;
		}
	}
	return found;
}

/* the command line's settings (HALO_SETTINGS, from the host: a line each,
"name=value", "name" - true - or "no-name" - false), over the file's and the
environment's; "help" lists them all */
static void config_apply_arguments(const char *text)
{
	while (text && *text)
	{
		char line[512], name[256];
		const char *end = strchr(text, '\n'), *value;
		size_t length = end ? (size_t)(end - text) : strlen(text), index;
		char *equals;
		long setting;

		if (length >= sizeof(line))
			length = sizeof(line) - 1;
		memcpy(line, text, length);
		line[length] = 0;
		text = end ? end + 1 : text + length;
		if (!line[0])
			continue;
		if (!strcmp(line, "help"))
		{
			for (index = 0; index < NUMBER_OF_CONFIG_SETTINGS; index++)
				fprintf(stderr, "--%s (%s, %s)\n    %s\n", config_settings[index].name,
					config_settings[index].default_value, config_settings[index].environment,
					config_settings[index].comment);
			fprintf(stderr, "short names: the last part of a name (--vsync=false), and --gi, --rt, --map, --fps, "
				"--windowed, --no-vsync, --mute\n");
			exit(0);
		}
		equals = strchr(line, '=');
		value = equals ? equals + 1 : "true";
		if (equals)
			*equals = 0;
		snprintf(name, sizeof(name), "%s", line);
		for (index = 0; name[index]; index++)
			if (name[index] == '-')
				name[index] = '_';
		setting = config_argument_index(name, &value);
		if (setting < 0 && !equals && !strncmp(name, "no_", 3))
		{
			value = "false";
			setting = config_argument_index(name + 3, &value);
		}
		if (setting == -2)
		{
			platform_log("settings: --%s is more than one setting's name; give it whole", line);
			continue;
		}
		if (setting < 0)
		{
			platform_log("settings: no setting --%s (--help lists them)", line);
			continue;
		}
		if (config_settings[setting].type == _config_string)
		{
			free(config_values[setting].string);
			config_values[setting].string = strdup(value);
		}
		else
			config_set_from_text(&config_values[setting], config_settings[setting].type, value);
		platform_log("settings: --%s: %s = %s", line, config_settings[setting].name, value);
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
		const char *environment = getenv(setting->environment);

		if (!environment)
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
	config_apply_arguments(getenv("HALO_SETTINGS"));
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

/* sets a setting's line in config.toml to "key = text", keeping the rest
of the file as it is: the line changed in place, or added at the end of its
section (or in a new section at the end); under config_lock */
static int config_write_line(const char *name, const char *text_value)
{
	const char *dot = strchr(name, '.');
	char section[64], key[64], current[64] = "", line_text[600], path[1024];
	struct config_text out = { 0 };
	size_t size = 0;
	char *text;
	const char *line;
	int written = 0, in_section = 0, succeeded;

	if (!dot || (size_t)(dot - name) >= sizeof(section))
		return 0;
	snprintf(section, sizeof(section), "%.*s", (int)(dot - name), name);
	snprintf(key, sizeof(key), "%s", dot + 1);
	snprintf(line_text, sizeof(line_text), "%s = %s\n", key, text_value);
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
			in_section = !strcmp(current, section);
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
	free(out.buffer);
	free(text);
	if (!succeeded)
		platform_log("settings: cannot write %s to %s", name, path);
	return succeeded;
}

/* the setting's index, if it is one of this type, with the file read */
static long config_writable(const char *name, enum config_type type)
{
	long index = config_setting_index(name);

	if (index < 0 || config_settings[index].type != type)
	{
		platform_log("settings: no %s setting %s to write", type == _config_string ? "string" : "such", name);
		return -1;
	}
	/* (the file read first, as the other settings are) */
	config_value(name, type);
	return index;
}

/* sets a boolean setting, for now and in config.toml: its line there is
changed (or added), the rest of the file kept as it is */
int config_write_boolean(const char *name, int value)
{
	long index = config_writable(name, _config_boolean);
	int succeeded;

	if (index < 0)
		return 0;
	pthread_mutex_lock(&config_lock);
	config_values[index].boolean = value != 0;
	succeeded = config_write_line(name, value ? "true" : "false");
	pthread_mutex_unlock(&config_lock);
	return succeeded;
}

int config_write_integer(const char *name, long value)
{
	long index = config_writable(name, _config_integer);
	char text[32];
	int succeeded;

	if (index < 0)
		return 0;
	snprintf(text, sizeof(text), "%ld", value);
	pthread_mutex_lock(&config_lock);
	config_values[index].integer = value;
	succeeded = config_write_line(name, text);
	pthread_mutex_unlock(&config_lock);
	return succeeded;
}

int config_write_real(const char *name, double value)
{
	long index = config_writable(name, _config_real);
	char text[48];
	int succeeded;

	if (index < 0)
		return 0;
	/* (TOML's floats need their point: 1.00, not 1) */
	snprintf(text, sizeof(text), "%.2f", value);
	pthread_mutex_lock(&config_lock);
	config_values[index].real = value;
	succeeded = config_write_line(name, text);
	pthread_mutex_unlock(&config_lock);
	return succeeded;
}

/* a number setting's value for now only, config.toml left as it is (a
slider being dragged: the settings overlay) */
void config_set_real(const char *name, double value)
{
	long index = config_writable(name, _config_real);

	if (index < 0)
		return;
	pthread_mutex_lock(&config_lock);
	config_values[index].real = value;
	pthread_mutex_unlock(&config_lock);
}

int config_write_string(const char *name, const char *value)
{
	long index = config_writable(name, _config_string);
	char text[520];
	size_t length = 0;
	int succeeded;

	if (index < 0)
		return 0;
	/* a TOML basic string: its quotes and backslashes escaped, no control
	characters */
	text[length++] = '"';
	for (; *value && length + 3 < sizeof(text); value++)
	{
		if ((unsigned char)*value < 0x20)
			continue;
		if (*value == '"' || *value == '\\')
			text[length++] = '\\';
		text[length++] = *value;
	}
	text[length++] = '"';
	text[length] = 0;
	pthread_mutex_lock(&config_lock);
	/* (the old text is not freed: config_string's callers on other threads
	may still be reading it, and a setting changes by hand a few times a
	run) */
	{
		char *copy = malloc(length);
		const char *from;
		size_t used = 0;

		/* (the value itself, unescaped) */
		for (from = text + 1; copy && from < text + length - 1; from++)
		{
			if (*from == '\\')
				from++;
			copy[used++] = *from;
		}
		if (copy)
		{
			copy[used] = 0;
			config_values[index].string = copy;
		}
	}
	succeeded = config_write_line(name, text);
	pthread_mutex_unlock(&config_lock);
	return succeeded;
}

/* the setting's comment in the table (a line break between its lines) */
const char *config_comment(const char *name)
{
	long index = config_setting_index(name);

	return index >= 0 ? config_settings[index].comment : "";
}

/* ---------- public code */

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

/* a map's console command: "map_name levels\\<name>\\<name>" for a campaign
level (a letter and two digits), "levels\\test\\<name>\\<name>" for a
multiplayer map, or the path as given; 0 for none */
int halo_map_command(const char *map, char *command, unsigned long size)
{
	size_t length = strlen(map);

	if (!length || length > 120)
		return 0;
	if (strchr(map, '\\') || strchr(map, '/'))
		snprintf(command, size, "map_name %s", map);
	else if (length == 3 && map[0] >= 'a' && map[0] <= 'd' && map[1] >= '0' && map[1] <= '9' && map[2] >= '0' &&
		map[2] <= '9')
		snprintf(command, size, "map_name levels\\%s\\%s", map, map);
	else
		snprintf(command, size, "map_name levels\\test\\%s\\%s", map, map);
	for (length = 0; command[length]; length++)
		if (command[length] == '/')
			command[length] = '\\';
	return 1;
}

/* game.map as a console command, or NULL for none */
const char *halo_startup_map_command(void)
{
	static char command[256];

	if (!halo_map_command(config_string("game.map"), command, sizeof(command)))
		return NULL;
	platform_log("game.map: %s", command);
	return command;
}

/* a console command for the game to run on its next update (the settings
overlay's map list: settings_overlay.c), before debug.commands' */
static char queued_command[256];

void halo_queue_command(const char *command)
{
	pthread_mutex_lock(&config_lock);
	snprintf(queued_command, sizeof(queued_command), "%s", command);
	pthread_mutex_unlock(&config_lock);
}

/* debug.commands: the next command whose time has come, once each, or NULL
(and halo_queue_command's first) */
const char *halo_timed_command_next(void)
{
	static int loaded;
	static int count, next;
	static struct { double at; char command[200]; } commands[32];
	static char current[256];
	double now;

	pthread_mutex_lock(&config_lock);
	if (queued_command[0])
	{
		snprintf(current, sizeof(current), "%s", queued_command);
		queued_command[0] = 0;
		pthread_mutex_unlock(&config_lock);
		platform_log("commands: %s", current);
		return current;
	}
	pthread_mutex_unlock(&config_lock);
	if (!loaded)
	{
		const char *text = config_string("debug.commands");

		loaded = 1;
		while (*text && count < 32)
		{
			char *end;
			double at = strtod(text, &end);
			size_t length = 0;

			if (end == text || *end != '=')
				break;
			end++;
			while (end[length] && end[length] != ';' && length < sizeof(commands[0].command) - 1)
				length++;
			commands[count].at = at;
			memcpy(commands[count].command, end, length);
			commands[count].command[length] = 0;
			count++;
			text = end + length;
			while (*text && *text != ';')
				text++;
			if (*text == ';')
				text++;
		}
		if (count)
			platform_log("commands: %d timed", count);
	}
	if (next >= count)
		return NULL;
	now = (double)SDL_GetTicks() / 1000.0;
	if (now < commands[next].at)
		return NULL;
	strcpy(current, commands[next].command);
	next++;
	platform_log("commands: %s", current);
	return current;
}
