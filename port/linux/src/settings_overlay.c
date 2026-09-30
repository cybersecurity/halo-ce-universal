/*
SETTINGS_OVERLAY.C

The settings overlay: F10 (Command-comma on a Mac) opens a panel over the
game with the settings a player changes while playing - the display, the
ray-traced lighting, the sound, the mouse - and a map to load. Up and down
(the mouse, the first controller's pad) choose a row; left and right (a
click) change it; Enter switches it. A change is taken up at once where the
game allows it (the rows marked "restart" wait for the next start) and
written to config.toml, where only that setting's line changes
(port_config.c). The selected row's help is the settings table's comment.
While it is open the game gets no keys, mouse or controller, and the mouse
is released (sdl_platform.c); F10 or Escape closes it.

It is drawn after the game's picture is put in the window (d3d8_gl.c's
Present), at the window's pixels, in one draw: rounded rectangles (a
distance to their edge worked out per pixel) and text, whose glyphs are the
IBM PC's 8x8 font (public domain, as SDL's debug text has it) smoothed to
32x32 (Scale2x twice) and kept as a distance field, so that it is sharp at
any size.
*/

#include "platform.h"
#include "sdl_platform.h"
#include "gl.h"
#include "port_config.h"
#include "raytrace_gl.h"
#include "settings_overlay.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HALO_ANDROID
/* (no keyboard to open it with) */
int settings_overlay_active(void) { return 0; }
void settings_overlay_set_active(int active) { (void)active; }
void settings_overlay_key(int scancode, int down, int repeat) { (void)scancode; (void)down; (void)repeat; }
void settings_overlay_mouse_motion(float x, float y) { (void)x; (void)y; }
void settings_overlay_mouse_button(int button, int down, float x, float y)
{
	(void)button; (void)down; (void)x; (void)y;
}
void settings_overlay_mouse_wheel(int steps) { (void)steps; }
void settings_overlay_gamepad(unsigned int buttons, int a, int b) { (void)buttons; (void)a; (void)b; }
void settings_overlay_update(void) {}
void settings_overlay_draw(int width, int height) { (void)width; (void)height; }
#else

/* the game's frames-a-second counter (source/main/main.c) */
extern unsigned char display_framerate;
/* dsound_sdl.c: audio.enabled and audio.volume now; whether there is sound */
int halo_audio_settings(int enabled, float volume);
/* xinput_sdl.c: the mouse's settings read again */
void halo_input_settings_changed(void);

/* ---------- the rows */

enum row_kind
{
	_row_heading,
	_row_boolean,
	_row_choice,
	_row_real,
	_row_action,
};

enum row_id
{
	_row_none,
	_row_resolution,
	_row_render_scale,
	_row_fullscreen,
	_row_vsync,
	_row_fps,
	_row_interpolation,
	_row_direct_camera,
	_row_ray_tracing,
	_row_ray_view,
	_row_ray_gi,
	_row_ray_lights,
	_row_ray_shapes,
	_row_ray_objects,
	_row_ray_occlusion,
	_row_ray_reflections,
	_row_ray_bounce,
	_row_ray_shadows,
	_row_ray_gi_sun,
	_row_ray_gi_bounce,
	_row_ray_gi_glow,
	_row_ray_bounces,
	_row_ray_samples,
	_row_ray_gi_lights,
	_row_ray_gi_split,
	_row_audio,
	_row_volume,
	_row_sensitivity,
	_row_invert,
	_row_aim_assist,
	_row_map,
	_row_load,
	_row_updates,
};

struct row
{
	enum row_kind kind;
	enum row_id id;
	const char *label;
	/* its config.toml name (its help is the table's comment), or NULL */
	const char *setting;
	/* choices' labels */
	const char *const *choices;
	int choice_count;
	/* a number's range and step; shown as a percentage */
	float minimum, maximum, step;
	int percent;
	/* taken up at the next start */
	int restart;
	/* help for a row with no setting */
	const char *note;
};

/* F8's resolutions (d3d8_gl.c), and display.resolution's text if it is
another */
static const char *const resolution_values[] = { "native", "2160p", "1440p", "1080p", "720p", "xbox" };
static const char *const resolution_labels[] = { "Native", "2160p", "1440p", "1080p", "720p", "Xbox 640x480", "" };
static char resolution_custom[64];

static const char *const tracing_labels[] = { "Full", "Lite (screen only)", "Off" };
static const char *const tracing_values[] = { "on", "screen", "off" };
static const char *const view_labels[] = { "Lighting", "Ray view", "Split", "Occlusion", "Depth" };
static const char *const gi_labels[] = { "Traced", "Black", "Path", "Off" };
static const char *const gi_values[] = { "traced", "black", "path", "off" };
static const char *const lights_labels[] = { "Traced", "Game" };
static const char *const lights_values[] = { "traced", "game" };
static const char *const shapes_labels[] = { "Model", "Collision" };
static const char *const shapes_values[] = { "model", "collision" };

/* the maps the game has (its maps folder is looked in when the overlay
opens) */
static const struct
{
	const char *name;
	const char *title;
} known_maps[] = {
	{ "a10", "The Pillar of Autumn" },
	{ "a30", "Halo" },
	{ "a50", "The Truth and Reconciliation" },
	{ "b30", "The Silent Cartographer" },
	{ "b40", "Assault on the Control Room" },
	{ "c10", "343 Guilty Spark" },
	{ "c20", "The Library" },
	{ "c40", "Two Betrayals" },
	{ "d20", "Keyes" },
	{ "d40", "The Maw" },
	{ "beavercreek", "Battle Creek" },
	{ "bloodgulch", "Blood Gulch" },
	{ "boardingaction", "Boarding Action" },
	{ "carousel", "Derelict" },
	{ "chillout", "Chill Out" },
	{ "damnation", "Damnation" },
	{ "hangemhigh", "Hang 'Em High" },
	{ "longest", "Longest" },
	{ "prisoner", "Prisoner" },
	{ "putput", "Chiron TL-34" },
	{ "ratrace", "Rat Race" },
	{ "sidewinder", "Sidewinder" },
	{ "wizard", "Wizard" },
};
#define KNOWN_MAP_COUNT ((int)(sizeof(known_maps) / sizeof(known_maps[0])))
static int map_indices[KNOWN_MAP_COUNT];
static int map_count;
static int map_selected;

static const struct row rows[] =
{
	{ _row_heading, _row_none, "Display" },
	{ _row_choice, _row_resolution, "Resolution", "display.resolution", resolution_labels, 6 },
	{ _row_real, _row_render_scale, "Render scale", "display.render_scale", NULL, 0, 0.5f, 2.0f, 0.25f },
	{ _row_boolean, _row_fullscreen, "Fullscreen", "display.fullscreen" },
	{ _row_boolean, _row_vsync, "Vertical sync", "display.vsync" },
	{ _row_boolean, _row_fps, "Frame rate counter", "display.show_fps" },
	{ _row_boolean, _row_interpolation, "Frame interpolation", "display.interpolation", NULL, 0, 0, 0, 0, 0, 1 },
	{ _row_boolean, _row_direct_camera, "Direct camera", "display.direct_camera" },

	{ _row_heading, _row_none, "Ray tracing" },
	{ _row_choice, _row_ray_tracing, "Ray tracing", "display.ray_tracing", tracing_labels, 3 },
	{ _row_choice, _row_ray_view, "View", NULL, view_labels, 5, 0, 0, 0, 0, 0,
		"What the ray tracing shows (F6 steps through it): the lighting; the ray view (what\n"
		"the rays hit, from the camera); the lighting and the ray view side by side; the\n"
		"traced occlusion; the depth. For looking, not saved." },
	{ _row_choice, _row_ray_gi, "Traced light", "display.ray_tracing_gi", gi_labels, 4 },
	{ _row_choice, _row_ray_lights, "Lights", "display.ray_tracing_lights", lights_labels, 2 },
	{ _row_choice, _row_ray_shapes, "Object shapes", "display.ray_tracing_shapes", shapes_labels, 2 },
	{ _row_boolean, _row_ray_objects, "Objects in the rays", "display.ray_tracing_objects" },
	{ _row_real, _row_ray_occlusion, "Occlusion", "display.ray_tracing_occlusion", NULL, 0, 0.0f, 1.0f, 0.05f },
	{ _row_real, _row_ray_reflections, "Reflections", "display.ray_tracing_reflections", NULL, 0, 0.0f, 1.0f, 0.05f },
	{ _row_real, _row_ray_bounce, "Bounce", "display.ray_tracing_bounce", NULL, 0, 0.0f, 1.0f, 0.05f },
	{ _row_real, _row_ray_shadows, "Shadows", "display.ray_tracing_shadows", NULL, 0, 0.0f, 1.0f, 0.05f },
	{ _row_real, _row_ray_gi_sun, "Sun", "display.ray_tracing_gi_sun", NULL, 0, 0.0f, 2.0f, 0.1f },
	{ _row_real, _row_ray_gi_bounce, "Bounced light", "display.ray_tracing_gi_bounce", NULL, 0, 0.0f, 2.0f, 0.1f },
	{ _row_real, _row_ray_gi_glow, "Glowing surfaces", "display.ray_tracing_gi_glow", NULL, 0, 0.0f, 2.0f, 0.1f },
	{ _row_real, _row_ray_bounces, "Bounces (path)", "display.ray_tracing_bounces", NULL, 0, 1.0f, 4.0f, 1.0f },
	{ _row_real, _row_ray_samples, "Rays per pixel", "display.ray_tracing_samples", NULL, 0, 1.0f, 8.0f, 1.0f },
	{ _row_real, _row_ray_gi_lights, "Lights' strength", "display.ray_tracing_gi_lights", NULL, 0, 0.0f, 2.0f, 0.1f },
	{ _row_boolean, _row_ray_gi_split, "Split: game | traced", "display.ray_tracing_gi_split", NULL, 0, 0, 0, 0, 0, 0,
		"The game's own light on the left half of the screen, the traced on the right, to\n"
		"compare them. For looking, not saved." },

	{ _row_heading, _row_none, "Sound" },
	{ _row_boolean, _row_audio, "Sound", "audio.enabled" },
	{ _row_real, _row_volume, "Volume", "audio.volume", NULL, 0, 0.0f, 1.0f, 0.05f, 1 },

	{ _row_heading, _row_none, "Mouse" },
	{ _row_real, _row_sensitivity, "Sensitivity", "input.mouse_sensitivity", NULL, 0, 0.1f, 4.0f, 0.1f },
	{ _row_boolean, _row_invert, "Invert", "input.invert_mouse" },
	{ _row_boolean, _row_aim_assist, "Aim assist", "input.mouse_aim_assist" },

	{ _row_heading, _row_none, "Game" },
	{ _row_choice, _row_map, "Map", NULL, NULL, 0, 0, 0, 0, 0, 0,
		"The map Load starts: the campaign's levels and the multiplayer maps in the\n"
		"game's maps folder. (game.map in config.toml starts one at start-up.)" },
	{ _row_action, _row_load, "Load the map", NULL, NULL, 0, 0, 0, 0, 0, 0,
		"Starts the map chosen above now, as the console's map_name does; the game in\n"
		"progress ends." },
	{ _row_boolean, _row_updates, "Look for updates", "update.auto", NULL, 0, 0, 0, 0, 0, 1 },
};
#define ROW_COUNT ((int)(sizeof(rows) / sizeof(rows[0])))

/* ---------- the overlay's state */

static struct
{
	int active;
	int selected;
	/* the list's first pixel shown (scrolled) */
	float scroll;
	/* a message under the help: what the last change did, until when */
	char status[160];
	Uint64 status_until;
	/* the mouse: where, and the number row it drags */
	float mouse_x, mouse_y;
	int mouse_known;
	int dragging;
	/* the controller's buttons last seen, and presses waiting for the
	window's thread */
	unsigned int pad_last;
	unsigned int pad_pressed;
	pthread_mutex_t pad_lock;
	/* the last drawing's layout, for the mouse */
	int width, height;
	float list_top, list_bottom, row_x0, row_x1;
	float row_top[ROW_COUNT], row_bottom[ROW_COUNT];
	float value_x0, value_x1, slider_x0, slider_x1, map_x0;
} overlay = { .pad_lock = PTHREAD_MUTEX_INITIALIZER, .dragging = -1 };

static void status_set(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void status_set(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(overlay.status, sizeof(overlay.status), format, arguments);
	va_end(arguments);
	overlay.status_until = SDL_GetTicks() + 4000;
}

/* ---------- the settings, as they are now */

static int choice_count(const struct row *row)
{
	switch (row->id)
	{
	case _row_resolution:
		return resolution_custom[0] ? 7 : 6;
	case _row_map:
		return map_count;
	default:
		return row->choice_count;
	}
}

static const char *choice_label(const struct row *row, int index)
{
	if (index < 0 || index >= choice_count(row))
		return "";
	if (row->id == _row_resolution && index == 6)
		return resolution_custom;
	if (row->id == _row_map)
		return known_maps[map_indices[index]].name;
	return row->choices[index];
}

static int string_index(const char *const *values, int count, const char *value)
{
	int index;

	for (index = 0; index < count; index++)
	{
		if (!strcmp(values[index], value))
			return index;
	}
	return -1;
}

/* a boolean's or a choice's value */
static int row_index(const struct row *row)
{
	struct halo_ray_tracing_settings ray;

	switch (row->id)
	{
	case _row_resolution:
	{
		const char *current = halo_screen_resolution_current();
		int index = string_index(resolution_values, 6, current);

		if (index >= 0)
			return index;
		snprintf(resolution_custom, sizeof(resolution_custom), "%s", current);
		return 6;
	}
	case _row_fullscreen:
		return platform_window_fullscreen(-1);
	case _row_fps:
		return display_framerate != 0;
	case _row_ray_tracing:
		halo_ray_tracing_get(&ray);
		return ray.tracing == 1 ? 0 : ray.tracing == 2 ? 1 : 2;
	case _row_ray_view:
		halo_ray_tracing_get(&ray);
		return ray.view;
	case _row_ray_gi:
		halo_ray_tracing_get(&ray);
		return ray.gi == 1 ? 0 : ray.gi == 2 ? 1 : ray.gi == 3 ? 2 : 3;
	case _row_ray_lights:
		halo_ray_tracing_get(&ray);
		return ray.traced_lights ? 0 : 1;
	case _row_ray_shapes:
		halo_ray_tracing_get(&ray);
		return ray.shapes;
	case _row_ray_objects:
		halo_ray_tracing_get(&ray);
		return ray.objects != 0;
	case _row_ray_gi_split:
		halo_ray_tracing_get(&ray);
		return ray.gi_split != 0;
	case _row_map:
		return map_selected;
	default:
		return row->setting && row->kind == _row_boolean ? config_boolean(row->setting) != 0 : 0;
	}
}

static float row_real(const struct row *row)
{
	struct halo_ray_tracing_settings ray;

	halo_ray_tracing_get(&ray);
	switch (row->id)
	{
	case _row_ray_occlusion: return ray.occlusion;
	case _row_ray_reflections: return ray.reflections;
	case _row_ray_bounce: return ray.bounce;
	case _row_ray_shadows: return ray.shadows;
	case _row_ray_gi_sun: return ray.gi_sun;
	case _row_ray_gi_bounce: return ray.gi_bounce;
	case _row_ray_gi_glow: return ray.gi_glow;
	case _row_ray_bounces: return ray.gi_bounces;
	case _row_ray_samples: return ray.gi_samples;
	case _row_ray_gi_lights: return ray.gi_lights;
	default:
		return row->setting ? (float)config_real(row->setting) : 0.0f;
	}
}

/* ---------- changing them */

static void saved(const struct row *row, int written, const char *value)
{
	if (!written)
		status_set("Could not write config.toml (see the log)");
	else if (row->restart)
		status_set("Saved %s = %s; it applies at the next start", row->setting, value);
	else
		status_set("Saved %s = %s", row->setting, value);
	platform_log("settings overlay: %s = %s", row->setting, value);
}

static void set_index(const struct row *row, int index)
{
	struct halo_ray_tracing_settings ray;
	int count = choice_count(row);

	if (row->kind == _row_choice && count > 0)
		index = ((index % count) + count) % count;
	if (row->kind == _row_boolean)
		index = index != 0;
	halo_ray_tracing_get(&ray);
	switch (row->id)
	{
	case _row_resolution:
	{
		const char *value = index < 6 ? resolution_values[index] : resolution_custom;

		saved(row, config_write_string(row->setting, value), value);
		halo_screen_resolution_set(value);
		return;
	}
	case _row_fullscreen:
		platform_window_fullscreen(index);
		break;
	case _row_vsync:
		platform_video_set_vsync(index);
		break;
	case _row_fps:
		display_framerate = (unsigned char)index;
		break;
	case _row_ray_tracing:
		ray.tracing = index == 0 ? 1 : index == 1 ? 2 : 0;
		halo_ray_tracing_set(&ray);
		halo_ray_tracing_get(&ray);
		saved(row, config_write_string(row->setting, tracing_values[index]), tracing_values[index]);
		if (index == 0 && ray.tracing != 1)
			status_set("Full ray tracing is not available here: lite (screen-space rays) only");
		return;
	case _row_ray_view:
		ray.view = index;
		if (!ray.tracing)
			ray.tracing = ray.hardware_available ? 1 : 2;
		halo_ray_tracing_set(&ray);
		status_set("View: %s (not saved)", view_labels[index]);
		return;
	case _row_ray_gi:
		ray.gi = index == 0 ? 1 : index == 1 ? 2 : index == 2 ? 3 : 0;
		halo_ray_tracing_set(&ray);
		saved(row, config_write_string(row->setting, gi_values[index]), gi_values[index]);
		return;
	case _row_ray_lights:
		ray.traced_lights = index == 0;
		halo_ray_tracing_set(&ray);
		saved(row, config_write_string(row->setting, lights_values[index]), lights_values[index]);
		return;
	case _row_ray_shapes:
		ray.shapes = index;
		halo_ray_tracing_set(&ray);
		saved(row, config_write_string(row->setting, shapes_values[index]), shapes_values[index]);
		return;
	case _row_ray_objects:
		ray.objects = index;
		halo_ray_tracing_set(&ray);
		break;
	case _row_ray_gi_split:
		/* (for comparing, as the view: not saved - left on, half the screen
		kept the game's light at the next start) */
		ray.gi_split = index;
		halo_ray_tracing_set(&ray);
		status_set(index ? "Split on: the game's light on the left, the traced on the right (not saved)" :
			"Split off (not saved)");
		return;
	case _row_map:
		map_selected = index;
		return;
	default:
		break;
	}
	if (row->kind == _row_boolean && row->setting)
	{
		saved(row, config_write_boolean(row->setting, index), index ? "true" : "false");
		/* (after the setting: they read it) */
		if (row->id == _row_audio && !halo_audio_settings(index, (float)config_real("audio.volume")) && index)
			status_set("Saved audio.enabled = true; the sound starts at the next start");
		if (row->id == _row_invert || row->id == _row_aim_assist)
			halo_input_settings_changed();
		if (row->id == _row_fullscreen && config_boolean("debug.hidden_window"))
			status_set("Saved display.fullscreen; a hidden window stays a window");
	}
}

/* a number, taken up at once and saved if save */
static void set_real(const struct row *row, float value, int save)
{
	struct halo_ray_tracing_settings ray;
	char text[32];

	if (value < row->minimum)
		value = row->minimum;
	if (value > row->maximum)
		value = row->maximum;
	/* (to the step, so the file has 0.35, not 0.350001) */
	value = row->minimum + floorf((value - row->minimum) / row->step + 0.5f) * row->step;
	value = floorf(value * 100.0f + 0.5f) / 100.0f;
	halo_ray_tracing_get(&ray);
	switch (row->id)
	{
	case _row_ray_occlusion: ray.occlusion = value; break;
	case _row_ray_reflections: ray.reflections = value; break;
	case _row_ray_bounce: ray.bounce = value; break;
	case _row_ray_shadows: ray.shadows = value; break;
	case _row_ray_gi_sun: ray.gi_sun = value; break;
	case _row_ray_gi_bounce: ray.gi_bounce = value; break;
	case _row_ray_gi_glow: ray.gi_glow = value; break;
	case _row_ray_bounces: ray.gi_bounces = value; break;
	case _row_ray_samples: ray.gi_samples = value; break;
	case _row_ray_gi_lights: ray.gi_lights = value; break;
	default: break;
	}
	halo_ray_tracing_set(&ray);
	snprintf(text, sizeof(text), "%.2f", value);
	if (save)
		saved(row, config_write_real(row->setting, value), text);
	else if (row->setting)
	{
		/* (dragged: in memory only, until the button is let go) */
		config_set_real(row->setting, value);
	}
	if (row->id == _row_volume)
		halo_audio_settings(config_boolean("audio.enabled"), value);
	if (row->id == _row_sensitivity)
		halo_input_settings_changed();
}

static void load_map(void)
{
	char command[256];
	const char *name;

	if (map_selected < 0 || map_selected >= map_count)
	{
		status_set("No maps found in the maps folder");
		return;
	}
	name = known_maps[map_indices[map_selected]].name;
	if (!halo_map_command(name, command, sizeof(command)))
		return;
	halo_queue_command(command);
	platform_log("settings overlay: loading %s (%s)", name, command);
	settings_overlay_set_active(0);
}

/* left (-1), right (1) or Enter (0) on the selected row */
static void row_change(int row_index_, int direction)
{
	const struct row *row;

	if (row_index_ < 0 || row_index_ >= ROW_COUNT)
		return;
	row = &rows[row_index_];
	switch (row->kind)
	{
	case _row_boolean:
		set_index(row, !row_index(row));
		break;
	case _row_choice:
		if (choice_count(row) > 0)
			set_index(row, row_index(row) + (direction ? direction : 1));
		break;
	case _row_real:
		if (direction)
			set_real(row, row_real(row) + row->step * (float)direction, 1);
		break;
	case _row_action:
		if (!direction && row->id == _row_load)
			load_map();
		break;
	default:
		break;
	}
}

/* the next row that is not a heading, from row index in direction */
static int row_step(int index, int direction)
{
	int next = index;

	do
	{
		next += direction;
		if (next < 0 || next >= ROW_COUNT)
			return index;
	} while (rows[next].kind == _row_heading);
	return next;
}

/* ---------- the maps */

static void maps_find(void)
{
	const char *root = platform_data_root();
	const char *current = config_string("game.map");
	int index;

	map_count = 0;
	for (index = 0; index < KNOWN_MAP_COUNT; index++)
	{
		char path[1024];
		FILE *file;

		snprintf(path, sizeof(path), "%s/maps/%s.map", root, known_maps[index].name);
		file = fopen(path, "rb");
		if (!file)
			continue;
		fclose(file);
		if (!strcmp(known_maps[index].name, current))
			map_selected = map_count;
		map_indices[map_count++] = index;
	}
	if (map_selected >= map_count)
		map_selected = 0;
}

/* ---------- opening and input */

int settings_overlay_active(void)
{
	return overlay.active;
}

void settings_overlay_set_active(int active)
{
	active = active != 0;
	if (active == overlay.active)
		return;
	overlay.active = active;
	overlay.dragging = -1;
	overlay.mouse_known = 0;
	if (active)
	{
		maps_find();
		if (overlay.selected <= 0 || overlay.selected >= ROW_COUNT || rows[overlay.selected].kind == _row_heading)
			overlay.selected = row_step(0, 1);
		overlay.status[0] = 0;
	}
	platform_log("settings overlay: %s", active ? "open" : "closed");
}

void settings_overlay_key(int scancode, int down, int repeat)
{
	if (!overlay.active || !down)
		return;
	switch (scancode)
	{
	case SDL_SCANCODE_ESCAPE:
	case SDL_SCANCODE_F10:
		if (!repeat)
			settings_overlay_set_active(0);
		break;
	case SDL_SCANCODE_UP:
	case SDL_SCANCODE_W:
		overlay.selected = row_step(overlay.selected, -1);
		break;
	case SDL_SCANCODE_DOWN:
	case SDL_SCANCODE_S:
	case SDL_SCANCODE_TAB:
		overlay.selected = row_step(overlay.selected, 1);
		break;
	case SDL_SCANCODE_PAGEUP:
	{
		int step;

		for (step = 0; step < 8; step++)
			overlay.selected = row_step(overlay.selected, -1);
		break;
	}
	case SDL_SCANCODE_PAGEDOWN:
	{
		int step;

		for (step = 0; step < 8; step++)
			overlay.selected = row_step(overlay.selected, 1);
		break;
	}
	case SDL_SCANCODE_HOME:
		overlay.selected = row_step(-1, 1);
		break;
	case SDL_SCANCODE_END:
		overlay.selected = row_step(ROW_COUNT, -1);
		break;
	case SDL_SCANCODE_LEFT:
	case SDL_SCANCODE_A:
		row_change(overlay.selected, -1);
		break;
	case SDL_SCANCODE_RIGHT:
	case SDL_SCANCODE_D:
		row_change(overlay.selected, 1);
		break;
	case SDL_SCANCODE_RETURN:
	case SDL_SCANCODE_KP_ENTER:
	case SDL_SCANCODE_SPACE:
		if (!repeat)
			row_change(overlay.selected, 0);
		break;
	default:
		break;
	}
}

/* the row under a point in the last drawing, or -1 */
static int row_at(float x, float y)
{
	int index;

	if (x < overlay.row_x0 || x >= overlay.row_x1 || y < overlay.list_top || y >= overlay.list_bottom)
		return -1;
	for (index = 0; index < ROW_COUNT; index++)
	{
		if (rows[index].kind != _row_heading && y >= overlay.row_top[index] && y < overlay.row_bottom[index])
			return index;
	}
	return -1;
}

/* a number row's value where the mouse is on its slider */
static void slider_drag(int index, float x, int save)
{
	const struct row *row = &rows[index];
	float fraction = overlay.slider_x1 > overlay.slider_x0 ?
		(x - overlay.slider_x0) / (overlay.slider_x1 - overlay.slider_x0) : 0.0f;

	fraction = fraction < 0.0f ? 0.0f : fraction > 1.0f ? 1.0f : fraction;
	set_real(row, row->minimum + (row->maximum - row->minimum) * fraction, save);
}

void settings_overlay_mouse_motion(float x, float y)
{
	int index;

	int moved;

	if (!overlay.active)
		return;
	/* (the first motion only says where the pointer is: the pointer put in
	the window's middle as the overlay opens must not choose the row there) */
	moved = overlay.mouse_known && (fabsf(x - overlay.mouse_x) > 1.0f || fabsf(y - overlay.mouse_y) > 1.0f);
	overlay.mouse_known = 1;
	overlay.mouse_x = x;
	overlay.mouse_y = y;
	if (overlay.dragging >= 0)
	{
		slider_drag(overlay.dragging, x, 0);
		return;
	}
	if (!moved)
		return;
	index = row_at(x, y);
	if (index >= 0)
		overlay.selected = index;
}

void settings_overlay_mouse_button(int button, int down, float x, float y)
{
	int index;
	const struct row *row;

	if (!overlay.active || button != SDL_BUTTON_LEFT)
		return;
	if (!down)
	{
		/* (the dragged number saved once, where it was let go) */
		if (overlay.dragging >= 0)
			slider_drag(overlay.dragging, x, 1);
		overlay.dragging = -1;
		return;
	}
	index = row_at(x, y);
	if (index < 0)
		return;
	overlay.selected = index;
	row = &rows[index];
	switch (row->kind)
	{
	case _row_choice:
	{
		/* the value's left half steps back, the rest on */
		float x0 = row->id == _row_map ? overlay.map_x0 : overlay.value_x0;

		row_change(index, x >= x0 && x < (x0 + overlay.value_x1) * 0.5f ? -1 : 1);
		break;
	}
	case _row_real:
		if (x >= overlay.slider_x0 - 8.0f && x <= overlay.slider_x1 + 8.0f)
		{
			overlay.dragging = index;
			slider_drag(index, x, 0);
		}
		break;
	default:
		row_change(index, 0);
		break;
	}
}

void settings_overlay_mouse_wheel(int steps)
{
	if (!overlay.active)
		return;
	for (; steps > 0; steps--)
		overlay.selected = row_step(overlay.selected, -1);
	for (; steps < 0; steps++)
		overlay.selected = row_step(overlay.selected, 1);
}

void settings_overlay_gamepad(unsigned int buttons, int a, int b)
{
	unsigned int now = buttons & (XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT |
		XINPUT_GAMEPAD_DPAD_RIGHT);

	/* (A and B as bits of their own) */
	now |= (a ? 0x10000u : 0u) | (b ? 0x20000u : 0u);
	pthread_mutex_lock(&overlay.pad_lock);
	overlay.pad_pressed |= now & ~overlay.pad_last;
	overlay.pad_last = now;
	pthread_mutex_unlock(&overlay.pad_lock);
}

/* debug.settings_script: "<seconds>=<key>,<key>;..." */
static void script_run(const char *keys)
{
	static const struct
	{
		const char *name;
		int scancode;
	} names[] = {
		{ "up", SDL_SCANCODE_UP }, { "down", SDL_SCANCODE_DOWN }, { "left", SDL_SCANCODE_LEFT },
		{ "right", SDL_SCANCODE_RIGHT }, { "enter", SDL_SCANCODE_RETURN }, { "escape", SDL_SCANCODE_ESCAPE },
		{ "pageup", SDL_SCANCODE_PAGEUP }, { "pagedown", SDL_SCANCODE_PAGEDOWN }, { "home", SDL_SCANCODE_HOME },
		{ "end", SDL_SCANCODE_END },
	};

	while (*keys)
	{
		char key[64];
		size_t length = strcspn(keys, ",");
		float x = 0.0f, y = 0.0f;
		int index, steps;

		snprintf(key, sizeof(key), "%.*s", (int)(length < sizeof(key) ? length : sizeof(key) - 1), keys);
		keys += length;
		if (*keys == ',')
			keys++;
		platform_log("settings overlay: script %s", key);
		if (!strcmp(key, "open"))
			settings_overlay_set_active(1);
		else if (!strcmp(key, "close"))
			settings_overlay_set_active(0);
		else if (sscanf(key, "move:%f:%f", &x, &y) == 2)
			settings_overlay_mouse_motion(x * overlay.width, y * overlay.height);
		else if (sscanf(key, "click:%f:%f", &x, &y) == 2)
		{
			settings_overlay_mouse_motion(x * overlay.width, y * overlay.height);
			settings_overlay_mouse_button(SDL_BUTTON_LEFT, 1, x * overlay.width, y * overlay.height);
			settings_overlay_mouse_button(SDL_BUTTON_LEFT, 0, x * overlay.width, y * overlay.height);
		}
		else if (sscanf(key, "wheel:%d", &steps) == 1)
			settings_overlay_mouse_wheel(steps);
		else
		{
			for (index = 0; index < (int)(sizeof(names) / sizeof(names[0])); index++)
			{
				if (!strcmp(key, names[index].name))
					settings_overlay_key(names[index].scancode, 1, 0);
			}
		}
	}
}

static void script_update(void)
{
	static int loaded, count, next;
	static struct
	{
		double at;
		char keys[160];
	} steps[32];
	double now;

	if (!loaded)
	{
		const char *text = config_string("debug.settings_script");

		loaded = 1;
		while (*text && count < 32)
		{
			char *end;
			double at = strtod(text, &end);
			size_t length;

			if (end == text || *end != '=')
				break;
			end++;
			length = strcspn(end, ";");
			steps[count].at = at;
			snprintf(steps[count].keys, sizeof(steps[count].keys), "%.*s",
				(int)(length < sizeof(steps[count].keys) ? length : sizeof(steps[count].keys) - 1), end);
			count++;
			text = end + length;
			if (*text == ';')
				text++;
		}
	}
	now = (double)SDL_GetTicks() / 1000.0;
	while (next < count && now >= steps[next].at)
		script_run(steps[next++].keys);
}

void settings_overlay_update(void)
{
	unsigned int pressed;

	script_update();
	pthread_mutex_lock(&overlay.pad_lock);
	pressed = overlay.pad_pressed;
	overlay.pad_pressed = 0;
	pthread_mutex_unlock(&overlay.pad_lock);
	if (!overlay.active)
		return;
	if (pressed & XINPUT_GAMEPAD_DPAD_UP)
		overlay.selected = row_step(overlay.selected, -1);
	if (pressed & XINPUT_GAMEPAD_DPAD_DOWN)
		overlay.selected = row_step(overlay.selected, 1);
	if (pressed & XINPUT_GAMEPAD_DPAD_LEFT)
		row_change(overlay.selected, -1);
	if (pressed & XINPUT_GAMEPAD_DPAD_RIGHT)
		row_change(overlay.selected, 1);
	if (pressed & 0x10000u)
		row_change(overlay.selected, 0);
	if (pressed & 0x20000u)
		settings_overlay_set_active(0);
}

/* ---------- the font

The IBM PC's 8x8 glyphs (public domain; the rows top to bottom, the lowest
bit the leftmost pixel), for ' ' to '~'. */

static const unsigned char font_8x8[95 * 8] =
{
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /*   */
	0x18, 0x3c, 0x3c, 0x18, 0x18, 0x00, 0x18, 0x00,  /* ! */
	0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* " */
	0x36, 0x36, 0x7f, 0x36, 0x7f, 0x36, 0x36, 0x00,  /* # */
	0x0c, 0x3e, 0x03, 0x1e, 0x30, 0x1f, 0x0c, 0x00,  /* $ */
	0x00, 0x63, 0x33, 0x18, 0x0c, 0x66, 0x63, 0x00,  /* % */
	0x1c, 0x36, 0x1c, 0x6e, 0x3b, 0x33, 0x6e, 0x00,  /* & */
	0x06, 0x06, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00,  /* ' */
	0x18, 0x0c, 0x06, 0x06, 0x06, 0x0c, 0x18, 0x00,  /* ( */
	0x06, 0x0c, 0x18, 0x18, 0x18, 0x0c, 0x06, 0x00,  /* ) */
	0x00, 0x66, 0x3c, 0xff, 0x3c, 0x66, 0x00, 0x00,  /* 0x2a */
	0x00, 0x0c, 0x0c, 0x3f, 0x0c, 0x0c, 0x00, 0x00,  /* + */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c, 0x06,  /* , */
	0x00, 0x00, 0x00, 0x3f, 0x00, 0x00, 0x00, 0x00,  /* - */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c, 0x00,  /* . */
	0x60, 0x30, 0x18, 0x0c, 0x06, 0x03, 0x01, 0x00,  /* 0x2f */
	0x3e, 0x63, 0x73, 0x7b, 0x6f, 0x67, 0x3e, 0x00,  /* 0 */
	0x0c, 0x0e, 0x0c, 0x0c, 0x0c, 0x0c, 0x3f, 0x00,  /* 1 */
	0x1e, 0x33, 0x30, 0x1c, 0x06, 0x33, 0x3f, 0x00,  /* 2 */
	0x1e, 0x33, 0x30, 0x1c, 0x30, 0x33, 0x1e, 0x00,  /* 3 */
	0x38, 0x3c, 0x36, 0x33, 0x7f, 0x30, 0x78, 0x00,  /* 4 */
	0x3f, 0x03, 0x1f, 0x30, 0x30, 0x33, 0x1e, 0x00,  /* 5 */
	0x1c, 0x06, 0x03, 0x1f, 0x33, 0x33, 0x1e, 0x00,  /* 6 */
	0x3f, 0x33, 0x30, 0x18, 0x0c, 0x0c, 0x0c, 0x00,  /* 7 */
	0x1e, 0x33, 0x33, 0x1e, 0x33, 0x33, 0x1e, 0x00,  /* 8 */
	0x1e, 0x33, 0x33, 0x3e, 0x30, 0x18, 0x0e, 0x00,  /* 9 */
	0x00, 0x0c, 0x0c, 0x00, 0x00, 0x0c, 0x0c, 0x00,  /* : */
	0x00, 0x0c, 0x0c, 0x00, 0x00, 0x0c, 0x0c, 0x06,  /* ; */
	0x18, 0x0c, 0x06, 0x03, 0x06, 0x0c, 0x18, 0x00,  /* < */
	0x00, 0x00, 0x3f, 0x00, 0x00, 0x3f, 0x00, 0x00,  /* = */
	0x06, 0x0c, 0x18, 0x30, 0x18, 0x0c, 0x06, 0x00,  /* > */
	0x1e, 0x33, 0x30, 0x18, 0x0c, 0x00, 0x0c, 0x00,  /* ? */
	0x3e, 0x63, 0x7b, 0x7b, 0x7b, 0x03, 0x1e, 0x00,  /* @ */
	0x0c, 0x1e, 0x33, 0x33, 0x3f, 0x33, 0x33, 0x00,  /* A */
	0x3f, 0x66, 0x66, 0x3e, 0x66, 0x66, 0x3f, 0x00,  /* B */
	0x3c, 0x66, 0x03, 0x03, 0x03, 0x66, 0x3c, 0x00,  /* C */
	0x1f, 0x36, 0x66, 0x66, 0x66, 0x36, 0x1f, 0x00,  /* D */
	0x7f, 0x46, 0x16, 0x1e, 0x16, 0x46, 0x7f, 0x00,  /* E */
	0x7f, 0x46, 0x16, 0x1e, 0x16, 0x06, 0x0f, 0x00,  /* F */
	0x3c, 0x66, 0x03, 0x03, 0x73, 0x66, 0x7c, 0x00,  /* G */
	0x33, 0x33, 0x33, 0x3f, 0x33, 0x33, 0x33, 0x00,  /* H */
	0x1e, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x1e, 0x00,  /* I */
	0x78, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1e, 0x00,  /* J */
	0x67, 0x66, 0x36, 0x1e, 0x36, 0x66, 0x67, 0x00,  /* K */
	0x0f, 0x06, 0x06, 0x06, 0x46, 0x66, 0x7f, 0x00,  /* L */
	0x63, 0x77, 0x7f, 0x7f, 0x6b, 0x63, 0x63, 0x00,  /* M */
	0x63, 0x67, 0x6f, 0x7b, 0x73, 0x63, 0x63, 0x00,  /* N */
	0x1c, 0x36, 0x63, 0x63, 0x63, 0x36, 0x1c, 0x00,  /* O */
	0x3f, 0x66, 0x66, 0x3e, 0x06, 0x06, 0x0f, 0x00,  /* P */
	0x1e, 0x33, 0x33, 0x33, 0x3b, 0x1e, 0x38, 0x00,  /* Q */
	0x3f, 0x66, 0x66, 0x3e, 0x36, 0x66, 0x67, 0x00,  /* R */
	0x1e, 0x33, 0x07, 0x0e, 0x38, 0x33, 0x1e, 0x00,  /* S */
	0x3f, 0x2d, 0x0c, 0x0c, 0x0c, 0x0c, 0x1e, 0x00,  /* T */
	0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x3f, 0x00,  /* U */
	0x33, 0x33, 0x33, 0x33, 0x33, 0x1e, 0x0c, 0x00,  /* V */
	0x63, 0x63, 0x63, 0x6b, 0x7f, 0x77, 0x63, 0x00,  /* W */
	0x63, 0x63, 0x36, 0x1c, 0x1c, 0x36, 0x63, 0x00,  /* X */
	0x33, 0x33, 0x33, 0x1e, 0x0c, 0x0c, 0x1e, 0x00,  /* Y */
	0x7f, 0x63, 0x31, 0x18, 0x4c, 0x66, 0x7f, 0x00,  /* Z */
	0x1e, 0x06, 0x06, 0x06, 0x06, 0x06, 0x1e, 0x00,  /* [ */
	0x03, 0x06, 0x0c, 0x18, 0x30, 0x60, 0x40, 0x00,  /* 0x5c */
	0x1e, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1e, 0x00,  /* ] */
	0x08, 0x1c, 0x36, 0x63, 0x00, 0x00, 0x00, 0x00,  /* ^ */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff,  /* _ */
	0x0c, 0x0c, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00,  /* ` */
	0x00, 0x00, 0x1e, 0x30, 0x3e, 0x33, 0x6e, 0x00,  /* a */
	0x07, 0x06, 0x06, 0x3e, 0x66, 0x66, 0x3b, 0x00,  /* b */
	0x00, 0x00, 0x1e, 0x33, 0x03, 0x33, 0x1e, 0x00,  /* c */
	0x38, 0x30, 0x30, 0x3e, 0x33, 0x33, 0x6e, 0x00,  /* d */
	0x00, 0x00, 0x1e, 0x33, 0x3f, 0x03, 0x1e, 0x00,  /* e */
	0x1c, 0x36, 0x06, 0x0f, 0x06, 0x06, 0x0f, 0x00,  /* f */
	0x00, 0x00, 0x6e, 0x33, 0x33, 0x3e, 0x30, 0x1f,  /* g */
	0x07, 0x06, 0x36, 0x6e, 0x66, 0x66, 0x67, 0x00,  /* h */
	0x0c, 0x00, 0x0e, 0x0c, 0x0c, 0x0c, 0x1e, 0x00,  /* i */
	0x30, 0x00, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1e,  /* j */
	0x07, 0x06, 0x66, 0x36, 0x1e, 0x36, 0x67, 0x00,  /* k */
	0x0e, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x1e, 0x00,  /* l */
	0x00, 0x00, 0x33, 0x7f, 0x7f, 0x6b, 0x63, 0x00,  /* m */
	0x00, 0x00, 0x1f, 0x33, 0x33, 0x33, 0x33, 0x00,  /* n */
	0x00, 0x00, 0x1e, 0x33, 0x33, 0x33, 0x1e, 0x00,  /* o */
	0x00, 0x00, 0x3b, 0x66, 0x66, 0x3e, 0x06, 0x0f,  /* p */
	0x00, 0x00, 0x6e, 0x33, 0x33, 0x3e, 0x30, 0x78,  /* q */
	0x00, 0x00, 0x3b, 0x6e, 0x66, 0x06, 0x0f, 0x00,  /* r */
	0x00, 0x00, 0x3e, 0x03, 0x1e, 0x30, 0x1f, 0x00,  /* s */
	0x08, 0x0c, 0x3e, 0x0c, 0x0c, 0x2c, 0x18, 0x00,  /* t */
	0x00, 0x00, 0x33, 0x33, 0x33, 0x33, 0x6e, 0x00,  /* u */
	0x00, 0x00, 0x33, 0x33, 0x33, 0x1e, 0x0c, 0x00,  /* v */
	0x00, 0x00, 0x63, 0x6b, 0x7f, 0x7f, 0x36, 0x00,  /* w */
	0x00, 0x00, 0x63, 0x36, 0x1c, 0x36, 0x63, 0x00,  /* x */
	0x00, 0x00, 0x33, 0x33, 0x33, 0x3e, 0x30, 0x1f,  /* y */
	0x00, 0x00, 0x3f, 0x19, 0x0c, 0x26, 0x3f, 0x00,  /* z */
	0x38, 0x0c, 0x0c, 0x07, 0x0c, 0x0c, 0x38, 0x00,  /* { */
	0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x00,  /* | */
	0x07, 0x0c, 0x0c, 0x38, 0x0c, 0x0c, 0x07, 0x00,  /* } */
	0x6e, 0x3b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* ~ */
};

/* each glyph smoothed to 32x32 and kept as a distance field (0.5 at the
edge, 1 inside) in a cell of 40x40 with room for the field around it */
#define GLYPH_SIZE 32
#define GLYPH_PAD 4
#define GLYPH_CELL (GLYPH_SIZE + 2 * GLYPH_PAD)
#define ATLAS_COLUMNS 16
#define ATLAS_ROWS 6
#define ATLAS_WIDTH (ATLAS_COLUMNS * GLYPH_CELL)
#define ATLAS_HEIGHT (ATLAS_ROWS * GLYPH_CELL)
#define FIELD_RANGE 4
/* the glyphs' spacing, of their size (the 8x8 glyphs are 7 wide at most) */
#define GLYPH_ADVANCE 0.85f

/* Scale2x (EPX): each pixel into four, the corners taking a neighbour's
where two neighbours agree, so diagonal steps become slopes */
static void scale2x(const unsigned char *in, int size, unsigned char *out)
{
	int x, y;

	for (y = 0; y < size; y++)
	{
		for (x = 0; x < size; x++)
		{
			unsigned char p = in[y * size + x];
			unsigned char a = y > 0 ? in[(y - 1) * size + x] : 0;
			unsigned char b = x + 1 < size ? in[y * size + x + 1] : 0;
			unsigned char c = x > 0 ? in[y * size + x - 1] : 0;
			unsigned char d = y + 1 < size ? in[(y + 1) * size + x] : 0;
			unsigned char *o = &out[(y * 2) * size * 2 + x * 2];

			o[0] = c == a && c != d && a != b ? a : p;
			o[1] = a == b && a != c && b != d ? b : p;
			o[size * 2] = d == c && d != b && c != a ? c : p;
			o[size * 2 + 1] = b == d && b != a && d != c ? d : p;
		}
	}
}

static unsigned char *font_atlas(void)
{
	unsigned char *atlas = calloc(ATLAS_WIDTH * ATLAS_HEIGHT, 1);
	int glyph;

	if (!atlas)
		return NULL;
	for (glyph = 0; glyph < 95; glyph++)
	{
		unsigned char pixels8[8 * 8], pixels16[16 * 16], cell[GLYPH_CELL * GLYPH_CELL];
		unsigned char pixels32[GLYPH_SIZE * GLYPH_SIZE];
		int x, y, left = (glyph % ATLAS_COLUMNS) * GLYPH_CELL, top = (glyph / ATLAS_COLUMNS) * GLYPH_CELL;

		for (y = 0; y < 8; y++)
			for (x = 0; x < 8; x++)
				pixels8[y * 8 + x] = (font_8x8[glyph * 8 + y] >> x) & 1;
		scale2x(pixels8, 8, pixels16);
		scale2x(pixels16, 16, pixels32);
		memset(cell, 0, sizeof(cell));
		for (y = 0; y < GLYPH_SIZE; y++)
			for (x = 0; x < GLYPH_SIZE; x++)
				cell[(y + GLYPH_PAD) * GLYPH_CELL + x + GLYPH_PAD] = pixels32[y * GLYPH_SIZE + x];
		/* the distance to the nearest pixel of the other kind, within the
		range: positive inside */
		for (y = 0; y < GLYPH_CELL; y++)
		{
			for (x = 0; x < GLYPH_CELL; x++)
			{
				int inside = cell[y * GLYPH_CELL + x], dx, dy;
				float nearest = (float)FIELD_RANGE + 0.5f, distance;

				for (dy = -FIELD_RANGE; dy <= FIELD_RANGE; dy++)
				{
					for (dx = -FIELD_RANGE; dx <= FIELD_RANGE; dx++)
					{
						int sx = x + dx, sy = y + dy;
						int other = sx >= 0 && sy >= 0 && sx < GLYPH_CELL && sy < GLYPH_CELL ?
							cell[sy * GLYPH_CELL + sx] : 0;

						if (other != inside)
						{
							float d = sqrtf((float)(dx * dx + dy * dy));

							if (d < nearest)
								nearest = d;
						}
					}
				}
				/* (the edge half way between the two pixels' centres) */
				distance = inside ? nearest - 0.5f : -(nearest - 0.5f);
				distance = 0.5f + distance / (2.0f * FIELD_RANGE);
				distance = distance < 0.0f ? 0.0f : distance > 1.0f ? 1.0f : distance;
				atlas[(top + y) * ATLAS_WIDTH + left + x] = (unsigned char)(distance * 255.0f + 0.5f);
			}
		}
	}
	return atlas;
}

/* ---------- drawing */

#ifdef HALO_GLES
#define OVERLAY_SHADER_HEADER "#version 300 es\nprecision highp float;\n"
#else
#define OVERLAY_SHADER_HEADER "#version 330 core\n"
#endif

static const char vertex_source[] =
	OVERLAY_SHADER_HEADER
	"layout(location = 0) in vec4 position_uv;\n"
	"layout(location = 1) in vec4 color;\n"
	"layout(location = 2) in vec4 shape;\n"
	"uniform vec2 screen;\n"
	"out vec2 uv;\n"
	"out vec4 tint;\n"
	"out vec4 box;\n"
	"void main()\n"
	"{\n"
	"	uv = position_uv.zw;\n"
	"	tint = color;\n"
	"	box = shape;\n"
	"	gl_Position = vec4(position_uv.x / screen.x * 2.0 - 1.0, 1.0 - position_uv.y / screen.y * 2.0, 0.0, 1.0);\n"
	"}\n";

/* box: a rectangle's half size and corner radius (uv: the pixel from its
centre), or w 1 for text (uv: the atlas's) */
static const char pixel_source[] =
	OVERLAY_SHADER_HEADER
	"in vec2 uv;\n"
	"in vec4 tint;\n"
	"in vec4 box;\n"
	"uniform sampler2D font;\n"
	"out vec4 result;\n"
	"void main()\n"
	"{\n"
	"	float field = texture(font, uv).r;\n"
	"	float soft = max(fwidth(field), 1e-4) * 0.7;\n"
	"	float glyph = smoothstep(0.5 - soft, 0.5 + soft, field);\n"
	"	vec2 q = abs(uv) - box.xy + box.z;\n"
	"	float edge = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - box.z;\n"
	"	float alpha = box.w > 0.5 ? glyph : clamp(0.5 - edge, 0.0, 1.0);\n"
	"	result = vec4(tint.rgb, tint.a * alpha);\n"
	"}\n";

struct vertex
{
	float x, y, u, v;
	float color[4];
	float shape[4];
};

static struct
{
	int ready, failed;
	GLuint program, texture, vertex_array, buffer;
	GLint screen, font;
	struct vertex *vertices;
	int count, capacity;
} gl;

static GLuint compile(GLenum type, const char *source)
{
	GLuint shader = glCreateShader(type);
	GLint status = 0;

	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);
	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if (!status)
	{
		char log[2048];

		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		platform_log("settings overlay: cannot compile its shader:\n%s", log);
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

static int gl_prepare(void)
{
	GLuint vertex, pixel;
	GLint status = 0;
	unsigned char *atlas;

	if (gl.ready || gl.failed)
		return gl.ready;
	gl.failed = 1;
	vertex = compile(GL_VERTEX_SHADER, vertex_source);
	pixel = compile(GL_FRAGMENT_SHADER, pixel_source);
	if (!vertex || !pixel)
		return 0;
	gl.program = glCreateProgram();
	glAttachShader(gl.program, vertex);
	glAttachShader(gl.program, pixel);
	glLinkProgram(gl.program);
	glDeleteShader(vertex);
	glDeleteShader(pixel);
	glGetProgramiv(gl.program, GL_LINK_STATUS, &status);
	if (!status)
	{
		char log[2048];

		glGetProgramInfoLog(gl.program, sizeof(log), NULL, log);
		platform_log("settings overlay: cannot link its shader:\n%s", log);
		return 0;
	}
	gl.screen = glGetUniformLocation(gl.program, "screen");
	gl.font = glGetUniformLocation(gl.program, "font");
	atlas = font_atlas();
	if (!atlas)
		return 0;
	glActiveTexture(GL_TEXTURE0);
	glGenTextures(1, &gl.texture);
	glBindTexture(GL_TEXTURE_2D, gl.texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, ATLAS_WIDTH, ATLAS_HEIGHT, 0, GL_RED, GL_UNSIGNED_BYTE, atlas);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	free(atlas);
	glGenVertexArrays(1, &gl.vertex_array);
	glGenBuffers(1, &gl.buffer);
	glBindVertexArray(gl.vertex_array);
	glBindBuffer(GL_ARRAY_BUFFER, gl.buffer);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(struct vertex), (const void *)0);
	glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(struct vertex), (const void *)(4 * sizeof(float)));
	glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(struct vertex), (const void *)(8 * sizeof(float)));
	gl.failed = 0;
	gl.ready = 1;
	return 1;
}

static void push_vertex(float x, float y, float u, float v, const float *color, const float *shape)
{
	struct vertex *vertex;

	if (gl.count == gl.capacity)
	{
		int capacity = gl.capacity ? gl.capacity * 2 : 6144;
		struct vertex *vertices = realloc(gl.vertices, (size_t)capacity * sizeof(*vertices));

		if (!vertices)
			return;
		gl.vertices = vertices;
		gl.capacity = capacity;
	}
	vertex = &gl.vertices[gl.count++];
	vertex->x = x;
	vertex->y = y;
	vertex->u = u;
	vertex->v = v;
	memcpy(vertex->color, color, sizeof(vertex->color));
	memcpy(vertex->shape, shape, sizeof(vertex->shape));
}

static void push_quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
	const float *color, const float *shape)
{
	push_vertex(x0, y0, u0, v0, color, shape);
	push_vertex(x1, y0, u1, v0, color, shape);
	push_vertex(x0, y1, u0, v1, color, shape);
	push_vertex(x1, y0, u1, v0, color, shape);
	push_vertex(x1, y1, u1, v1, color, shape);
	push_vertex(x0, y1, u0, v1, color, shape);
}

/* a rectangle with rounded corners (a pixel wider each way, for its edge's
smoothing) */
static void draw_box(float x, float y, float width, float height, float radius, const float *color)
{
	float shape[4];

	if (width <= 0.0f || height <= 0.0f)
		return;
	shape[0] = width * 0.5f;
	shape[1] = height * 0.5f;
	shape[2] = radius < shape[0] && radius < shape[1] ? radius : (shape[0] < shape[1] ? shape[0] : shape[1]);
	shape[3] = 0.0f;
	push_quad(x - 1.0f, y - 1.0f, x + width + 1.0f, y + height + 1.0f, -shape[0] - 1.0f, -shape[1] - 1.0f,
		shape[0] + 1.0f, shape[1] + 1.0f, color, shape);
}

/* text from x, its cell's top at y, size pixels a glyph; its width */
static float draw_text(float x, float y, float size, const char *text, const float *color)
{
	static const float shape[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	float pad = size * GLYPH_PAD / GLYPH_SIZE, start = x;

	for (; *text; text++, x += size * GLYPH_ADVANCE)
	{
		int glyph = (unsigned char)*text - 32;
		float u, v;

		if (glyph <= 0 || glyph >= 95)
			continue;
		u = (float)((glyph % ATLAS_COLUMNS) * GLYPH_CELL) / ATLAS_WIDTH;
		v = (float)((glyph / ATLAS_COLUMNS) * GLYPH_CELL) / ATLAS_HEIGHT;
		push_quad(x - pad, y - pad, x + size + pad, y + size + pad, u, v, u + (float)GLYPH_CELL / ATLAS_WIDTH,
			v + (float)GLYPH_CELL / ATLAS_HEIGHT, color, shape);
	}
	return x - start;
}

static float text_width(float size, const char *text)
{
	return size * GLYPH_ADVANCE * (float)strlen(text);
}

/* the colours */
static const float color_panel[4] = { 0.035f, 0.047f, 0.063f, 0.94f };
static const float color_title[4] = { 0.07f, 0.10f, 0.13f, 0.95f };
static const float color_line[4] = { 1.0f, 1.0f, 1.0f, 0.07f };
static const float color_text[4] = { 0.92f, 0.94f, 0.96f, 1.0f };
static const float color_dim[4] = { 0.58f, 0.63f, 0.68f, 1.0f };
static const float color_faint[4] = { 0.40f, 0.44f, 0.48f, 1.0f };
static const float color_accent[4] = { 0.36f, 0.78f, 0.98f, 1.0f };
static const float color_select[4] = { 0.36f, 0.78f, 0.98f, 0.14f };
static const float color_track[4] = { 1.0f, 1.0f, 1.0f, 0.13f };
static const float color_restart[4] = { 0.98f, 0.72f, 0.30f, 1.0f };
static const float color_shade[4] = { 0.0f, 0.0f, 0.0f, 0.35f };

/* the help's text wrapped to columns, from y; returns the y after it */
static float draw_wrapped(float x, float y, float size, float line_height, int columns, int max_lines,
	const char *text, const float *color)
{
	char line[256];
	int lines = 0;

	if (columns > (int)sizeof(line) - 1)
		columns = (int)sizeof(line) - 1;
	while (*text && lines < max_lines)
	{
		int length = 0, fit = 0, index;

		while (*text == ' ' || *text == '\n')
			text++;
		/* the most words that fit */
		for (index = 0; text[index] && index <= columns; index++)
		{
			if (text[index] == ' ' || text[index] == '\n')
				fit = index;
			if (index == columns)
				break;
		}
		if (!text[index] && index <= columns)
			fit = index;
		if (!fit)
			fit = index < columns ? index : columns;
		for (length = 0; length < fit; length++)
			line[length] = text[length] == '\n' ? ' ' : text[length];
		line[length] = 0;
		text += fit;
		draw_text(x, y, size, line, color);
		y += line_height;
		lines++;
	}
	return y;
}

void settings_overlay_draw(int width, int height)
{
	float unit, size, margin, panel_x, panel_y, panel_w, panel_h, title_h, footer_h, list_x, list_w, help_x, help_w;
	float row_h, heading_h, y, total, content_top, content_bottom, radius;
	int index;
	char text[160];

	if (!overlay.active || width <= 0 || height <= 0 || !gl_prepare())
		return;
	overlay.width = width;
	overlay.height = height;
	gl.count = 0;

	/* the sizes: a glyph of 15 pixels in a 1080-line window, more on a
	Retina display's pixels */
	unit = (float)height / 1080.0f < (float)width / 1500.0f ? (float)height / 1080.0f : (float)width / 1500.0f;
	if (unit < 0.55f)
		unit = 0.55f;
	size = floorf(15.0f * unit + 0.5f);
	margin = size * 2.0f;
	panel_w = (float)width - 2.0f * margin;
	if (panel_w > size * 92.0f)
		panel_w = size * 92.0f;
	panel_h = (float)height - 2.0f * margin;
	if (panel_h > size * 62.0f)
		panel_h = size * 62.0f;
	panel_x = floorf(((float)width - panel_w) * 0.5f);
	panel_y = floorf(((float)height - panel_h) * 0.5f);
	radius = size * 0.8f;
	title_h = floorf(size * 3.6f);
	footer_h = floorf(size * 2.6f);
	row_h = floorf(size * 2.0f);
	heading_h = floorf(size * 2.6f);
	list_x = panel_x + size;
	list_w = floorf(panel_w * 0.58f) - size;
	help_x = list_x + list_w + size * 2.0f;
	help_w = panel_x + panel_w - size * 1.5f - help_x;

	/* the game darkened behind the panel, the panel, its title */
	draw_box(0.0f, 0.0f, (float)width, (float)height, 0.0f, color_shade);
	draw_box(panel_x, panel_y, panel_w, panel_h, radius, color_panel);
	draw_box(panel_x, panel_y, panel_w, title_h, radius, color_title);
	draw_box(panel_x, panel_y + title_h - radius, panel_w, radius, 0.0f, color_title);
	draw_box(panel_x, panel_y + title_h, panel_w, fmaxf(1.0f, floorf(unit)), 0.0f, color_line);
	draw_box(panel_x + size * 1.5f, panel_y + (title_h - size * 1.4f) * 0.5f, size * 0.3f, size * 1.4f,
		size * 0.15f, color_accent);
	draw_text(panel_x + size * 2.4f, panel_y + (title_h - size * 1.4f) * 0.5f, size * 1.4f, "Settings", color_text);
	{
		const char *hint = "F10 or Esc closes";

		draw_text(panel_x + panel_w - size * 1.5f - text_width(size * 0.8f, hint),
			panel_y + (title_h - size * 0.8f) * 0.5f, size * 0.8f, hint, color_faint);
	}

	/* the list, scrolled to keep the selected row in view */
	content_top = panel_y + title_h + size * 0.6f;
	content_bottom = panel_y + panel_h - footer_h;
	total = 0.0f;
	for (index = 0; index < ROW_COUNT; index++)
		total += rows[index].kind == _row_heading ? heading_h : row_h;
	{
		float top = 0.0f, bottom, view = content_bottom - content_top;

		for (index = 0; index < overlay.selected; index++)
			top += rows[index].kind == _row_heading ? heading_h : row_h;
		bottom = top + row_h;
		/* (the heading above the first row of a group shown with it) */
		if (overlay.selected > 0 && rows[overlay.selected - 1].kind == _row_heading)
			top -= heading_h;
		if (top < overlay.scroll + row_h * 0.5f)
			overlay.scroll = top - row_h * 0.5f;
		if (bottom > overlay.scroll + view - row_h * 0.5f)
			overlay.scroll = bottom - view + row_h * 0.5f;
		if (overlay.scroll > total - view)
			overlay.scroll = total - view;
		if (overlay.scroll < 0.0f)
			overlay.scroll = 0.0f;
		/* the scroll bar */
		if (total > view)
		{
			float bar_h = view * view / total, bar_y = content_top + (view - bar_h) * overlay.scroll / (total - view);

			draw_box(list_x + list_w + size * 0.6f, content_top, size * 0.25f, view, size * 0.12f, color_line);
			draw_box(list_x + list_w + size * 0.6f, bar_y, size * 0.25f, bar_h, size * 0.12f, color_track);
		}
	}
	overlay.list_top = content_top;
	overlay.list_bottom = content_bottom;
	overlay.row_x0 = list_x;
	overlay.row_x1 = list_x + list_w;
	overlay.value_x0 = list_x + list_w - size * 17.0f;
	overlay.map_x0 = list_x + size * 7.0f;
	overlay.value_x1 = list_x + list_w - size * 0.8f;
	overlay.slider_x0 = overlay.value_x0 + size * 0.4f;
	overlay.slider_x1 = overlay.value_x1 - size * 5.0f;
	y = content_top - overlay.scroll;
	for (index = 0; index < ROW_COUNT; index++)
	{
		const struct row *row = &rows[index];
		float height_here = row->kind == _row_heading ? heading_h : row_h;
		int selected = index == overlay.selected;
		float text_y = y + (row_h - size) * 0.5f;

		overlay.row_top[index] = y;
		overlay.row_bottom[index] = y + height_here;
		/* (only rows wholly in the list's space) */
		if (y < content_top - 0.5f || y + height_here > content_bottom + 0.5f)
		{
			y += height_here;
			continue;
		}
		if (row->kind == _row_heading)
		{
			float heading_y = y + heading_h - size * 0.8f - size * 0.55f;
			char upper[64];
			size_t k;

			for (k = 0; row->label[k] && k + 1 < sizeof(upper); k++)
				upper[k] = (char)(row->label[k] >= 'a' && row->label[k] <= 'z' ? row->label[k] - 32 : row->label[k]);
			upper[k] = 0;
			draw_text(list_x + size * 0.6f, heading_y, size * 0.8f, upper, color_accent);
			draw_box(list_x + size * 1.4f + text_width(size * 0.8f, upper), heading_y + size * 0.4f,
				list_w - size * 2.0f - text_width(size * 0.8f, upper), fmaxf(1.0f, floorf(unit)), 0.0f, color_line);
			y += height_here;
			continue;
		}
		if (selected)
		{
			draw_box(list_x, y + 1.0f, list_w, row_h - 2.0f, size * 0.35f, color_select);
			draw_box(list_x, y + size * 0.35f, size * 0.22f, row_h - size * 0.7f, size * 0.11f, color_accent);
		}
		draw_text(list_x + size * 1.2f, text_y, size, row->label, selected ? color_text : color_dim);
		if (row->restart)
			draw_text(list_x + size * 1.9f + text_width(size, row->label), text_y + size * 0.2f, size * 0.7f,
				"restart", color_restart);
		switch (row->kind)
		{
		case _row_boolean:
		{
			int on = row_index(row);
			float pill_w = size * 2.6f, pill_h = size * 1.3f;
			float pill_x = overlay.value_x1 - pill_w, pill_y = y + (row_h - pill_h) * 0.5f;
			float knob = pill_h - size * 0.4f;
			const char *word = on ? "On" : "Off";

			draw_box(pill_x, pill_y, pill_w, pill_h, pill_h * 0.5f, on ? color_accent : color_track);
			draw_box(on ? pill_x + pill_w - knob - size * 0.2f : pill_x + size * 0.2f, pill_y + size * 0.2f, knob,
				knob, knob * 0.5f, on ? color_text : color_dim);
			draw_text(pill_x - size * 0.8f - text_width(size, word), text_y, size, word,
				on ? color_text : color_dim);
			break;
		}
		case _row_choice:
		{
			int count = choice_count(row), current = row_index(row);
			const char *label = count ? choice_label(row, current) : "none";
			/* (the map's the row's width after its label: its title is long) */
			float left = row->id == _row_map ? overlay.map_x0 : overlay.value_x0;
			float middle = (left + overlay.value_x1) * 0.5f;

			if (row->id == _row_map && count)
			{
				snprintf(text, sizeof(text), "%s  %s", known_maps[map_indices[current]].name,
					known_maps[map_indices[current]].title);
				label = text;
				if (text_width(size, label) > overlay.value_x1 - left - size * 3.0f)
					label = choice_label(row, current);
			}
			draw_text(left, text_y, size, "<", selected ? color_accent : color_faint);
			draw_text(overlay.value_x1 - size, text_y, size, ">", selected ? color_accent : color_faint);
			draw_text(floorf(middle - text_width(size, label) * 0.5f), text_y, size, label, color_text);
			break;
		}
		case _row_real:
		{
			float value = row_real(row);
			float fraction = (value - row->minimum) / (row->maximum - row->minimum);
			float track_y = y + row_h * 0.5f - size * 0.15f, knob = size * 0.9f;
			float knob_x = overlay.slider_x0 + (overlay.slider_x1 - overlay.slider_x0) * fraction;

			fraction = fraction < 0.0f ? 0.0f : fraction > 1.0f ? 1.0f : fraction;
			knob_x = overlay.slider_x0 + (overlay.slider_x1 - overlay.slider_x0) * fraction;
			draw_box(overlay.slider_x0, track_y, overlay.slider_x1 - overlay.slider_x0, size * 0.3f, size * 0.15f,
				color_track);
			draw_box(overlay.slider_x0, track_y, knob_x - overlay.slider_x0, size * 0.3f, size * 0.15f, color_accent);
			draw_box(knob_x - knob * 0.5f, y + (row_h - knob) * 0.5f, knob, knob, knob * 0.5f,
				selected ? color_text : color_dim);
			if (row->percent)
				snprintf(text, sizeof(text), "%d%%", (int)floorf(value * 100.0f + 0.5f));
			else
				snprintf(text, sizeof(text), "%.2f", value);
			draw_text(overlay.value_x1 - text_width(size, text), text_y, size, text, color_text);
			break;
		}
		case _row_action:
		{
			const char *word = "Load";
			float button_w = text_width(size, word) + size * 2.0f, button_h = size * 1.5f;

			draw_box(overlay.value_x1 - button_w, y + (row_h - button_h) * 0.5f, button_w, button_h, size * 0.4f,
				selected ? color_accent : color_track);
			draw_text(overlay.value_x1 - button_w + size, text_y, size, word, selected ? color_panel : color_text);
			break;
		}
		default:
			break;
		}
		y += height_here;
	}

	/* the help: the selected row's name, what it does, where it is saved */
	draw_box(help_x - size * 1.0f, content_top, fmaxf(1.0f, floorf(unit)), content_bottom - content_top - size * 0.6f,
		0.0f, color_line);
	if (overlay.selected >= 0 && overlay.selected < ROW_COUNT)
	{
		const struct row *row = &rows[overlay.selected];
		float help_size = floorf(size * 0.87f + 0.5f);
		int columns = (int)(help_w / (help_size * GLYPH_ADVANCE));
		const char *help = row->setting ? config_comment(row->setting) : row->note ? row->note : "";
		float help_y = content_top + size * 0.4f;

		draw_text(help_x, help_y, size * 1.2f, row->label, color_text);
		help_y += size * 2.4f;
		help_y = draw_wrapped(help_x, help_y, help_size, help_size * 1.55f, columns, 16, help, color_dim);
		help_y += size * 1.0f;
		if (row->restart)
		{
			help_y = draw_wrapped(help_x, help_y, help_size, help_size * 1.55f, columns, 2,
				"Applies at the next start.", color_restart);
			help_y += size * 0.6f;
		}
		if (row->setting)
		{
			snprintf(text, sizeof(text), "config.toml: %s", row->setting);
			help_y = draw_wrapped(help_x, help_y, help_size * 0.9f, help_size * 1.5f, (int)(help_w / (help_size * 0.9f * GLYPH_ADVANCE)),
				2, text, color_faint);
		}
		if (overlay.status[0] && SDL_GetTicks() < overlay.status_until)
		{
			help_y += size * 1.0f;
			draw_wrapped(help_x, help_y, help_size, help_size * 1.55f, columns, 3, overlay.status, color_accent);
		}
	}

	/* the keys */
	draw_box(panel_x, panel_y + panel_h - footer_h, panel_w, fmaxf(1.0f, floorf(unit)), 0.0f, color_line);
	draw_text(panel_x + size * 1.5f, panel_y + panel_h - footer_h + (footer_h - size * 0.8f) * 0.5f, size * 0.8f,
		"Up/Down: choose    Left/Right: change    Enter: switch    Mouse: point, click, drag    Esc: close",
		color_faint);

	/* one draw, over the window's picture */
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, width, height);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_POLYGON_OFFSET_FILL);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glEnable(GL_BLEND);
	glBlendEquation(GL_FUNC_ADD);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glUseProgram(gl.program);
	glUniform2f(gl.screen, (float)width, (float)height);
	glUniform1i(gl.font, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, gl.texture);
	glBindSampler(0, 0);
	glBindVertexArray(gl.vertex_array);
	glBindBuffer(GL_ARRAY_BUFFER, gl.buffer);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)gl.count * sizeof(struct vertex)), gl.vertices, GL_STREAM_DRAW);
	glDrawArrays(GL_TRIANGLES, 0, gl.count);
	glDisable(GL_BLEND);
}

#endif
