/*
GAME_SETTINGS.C

The settings of the menus' VIDEO, AUDIO and INPUT SETTINGS
(port/linux/include/game_settings.h).
A change is made in the running game first, by the part of the port that
uses the setting, and only then written into config.toml: what the driver
or the window system refuses is neither shown nor kept. When the file cannot
be written the change still holds for this run, and the log says so.

A number steps through a range on a grid of its step, which a value set in
the file between two steps joins. The resolution steps through the sizes
the display has (platform_display_resolutions), and is two settings in the
file, a width and a height. F11 still switches fullscreen for the run only;
VIDEO SETTINGS shows the window as it is.
*/

#include "platform.h"
#include "sdl_platform.h"
#include "port_config.h"
#include "game_settings.h"

#include <math.h>
#include <stdio.h>

#ifdef HALO_ANDROID

int game_settings_available(void)
{
	return 0;
}

void game_setting_text(int setting, char *text, int size)
{
	(void)setting;
	if (size > 0)
		text[0] = 0;
}

int game_setting_is_switch(int setting)
{
	(void)setting;
	return 1;
}

int game_setting_step(int setting, int direction)
{
	(void)setting;
	(void)direction;
	return 0;
}

#else

/* port/linux/game/render_interpolation.c */
int render_interpolation_direct_camera_enabled(void);
void render_interpolation_set_direct_camera(int enabled);

enum game_setting_type
{
	_game_setting_type_switch,
	_game_setting_type_integer,
	/* a real shown as a percentage */
	_game_setting_type_percentage,
	_game_setting_type_real,
	/* a width and a height of the display's (platform_fullscreen_resolution) */
	_game_setting_type_resolution,
	/* a whole number that steps by doubling and halving, shown as 2X, 4X and
	so on, and its minimum (1) as OFF */
	_game_setting_type_multiple,
};

struct game_setting
{
	const char *name;
	enum game_setting_type type;
	/* a number's range (window_scale's top is the display's,
	anisotropic_filtering's the GPU's: game_setting_maximum) and step */
	double minimum, maximum, step;
};

static const struct game_setting game_settings[NUMBER_OF_GAME_SETTINGS] =
{
	{ "display.fullscreen", _game_setting_type_switch, 0.0, 0.0, 0.0 },
	{ "display.resolution", _game_setting_type_resolution, 0.0, 0.0, 0.0 },
	{ "display.vsync", _game_setting_type_switch, 0.0, 0.0, 0.0 },
	{ "display.interpolation", _game_setting_type_switch, 0.0, 0.0, 0.0 },
	{ "display.direct_camera", _game_setting_type_switch, 0.0, 0.0, 0.0 },
	{ "display.window_scale", _game_setting_type_integer, 1.0, 0.0, 1.0 },
	{ "display.anisotropic_filtering", _game_setting_type_multiple, 1.0, 0.0, 0.0 },
	{ "audio.enabled", _game_setting_type_switch, 0.0, 0.0, 0.0 },
	{ "audio.volume", _game_setting_type_percentage, 0.0, 1.0, 0.1 },
	{ "input.mouse_sensitivity", _game_setting_type_real, 0.1, 10.0, 0.1 },
	{ "input.mouse_aim_assist", _game_setting_type_switch, 0.0, 0.0, 0.0 },
	{ "input.invert_mouse", _game_setting_type_switch, 0.0, 0.0, 0.0 },
};

int game_settings_available(void)
{
	return 1;
}

static double game_setting_value(int setting)
{
	switch (setting)
	{
	case _game_setting_fullscreen:
		return platform_fullscreen();
	case _game_setting_vsync:
		return platform_vsync();
	case _game_setting_interpolation:
		return halo_interpolation_enabled();
	case _game_setting_direct_camera:
		return render_interpolation_direct_camera_enabled();
	case _game_setting_window_scale:
		return platform_window_scale();
	case _game_setting_anisotropic_filtering:
		return video_anisotropic_filtering();
	case _game_setting_audio:
		return audio_output_enabled();
	case _game_setting_master_volume:
		return audio_master_volume();
	case _game_setting_mouse_sensitivity:
		return input_mouse_sensitivity();
	case _game_setting_mouse_aim_assist:
		return input_mouse_aim_assist();
	case _game_setting_invert_mouse:
		return input_mouse_inverted();
	}
	return 0.0;
}

/* FALSE if the setting could not be changed, and is as it was */
static BOOL game_setting_apply(int setting, double value)
{
	switch (setting)
	{
	case _game_setting_fullscreen:
		return platform_set_fullscreen(value != 0.0, TRUE) && platform_fullscreen() == (value != 0.0);
	case _game_setting_vsync:
		return platform_set_vsync(value != 0.0);
	case _game_setting_interpolation:
		halo_interpolation_set_enabled(value != 0.0);
		return TRUE;
	case _game_setting_direct_camera:
		render_interpolation_set_direct_camera(value != 0.0);
		return TRUE;
	case _game_setting_window_scale:
		return platform_set_window_scale((int)value);
	case _game_setting_anisotropic_filtering:
		video_set_anisotropic_filtering((int)value);
		return TRUE;
	case _game_setting_audio:
		return audio_set_output_enabled(value != 0.0);
	case _game_setting_master_volume:
		audio_set_master_volume((float)value);
		return TRUE;
	case _game_setting_mouse_sensitivity:
		input_set_mouse_sensitivity((float)value);
		return TRUE;
	case _game_setting_mouse_aim_assist:
		input_set_mouse_aim_assist(value != 0.0);
		return TRUE;
	case _game_setting_invert_mouse:
		input_set_mouse_inverted(value != 0.0);
		return TRUE;
	}
	return FALSE;
}

/* the top of a number's range */
static double game_setting_maximum(int setting)
{
	switch (setting)
	{
	case _game_setting_window_scale:
		return platform_window_scale_maximum();
	case _game_setting_anisotropic_filtering:
		return video_anisotropic_filtering_maximum();
	}
	return game_settings[setting].maximum;
}

/* how a change was kept, for the log */
static void game_setting_log_written(const char *name, const char *text, int written)
{
	if (written)
		platform_log("settings: %s set to %s", name, text);
	else
		platform_log("settings: %s set to %s for this run only: cannot write config.toml", name, text);
}

/* ---------- the resolution */

#define MAXIMUM_DISPLAY_RESOLUTIONS 64

static void game_setting_resolution_format(int width, int height, char *text, int size)
{
	snprintf(text, (size_t)size, "%d X %d", width, height);
}

/* the sizes in order: by their width, then their height */
static int game_setting_resolution_compare(int width, int height, int other_width, int other_height)
{
	return width != other_width ? width - other_width : height - other_height;
}

/* the size of the display's next larger (direction > 0) or smaller than the
resolution now; FALSE at the end */
static BOOL game_setting_resolution_next(int direction, int *width, int *height)
{
	int widths[MAXIMUM_DISPLAY_RESOLUTIONS], heights[MAXIMUM_DISPLAY_RESOLUTIONS];
	int count = platform_display_resolutions(widths, heights, MAXIMUM_DISPLAY_RESOLUTIONS);
	int current_width, current_height;
	int index;
	BOOL found = FALSE;

	platform_fullscreen_resolution(&current_width, &current_height);
	for (index = 0; index < count; index++)
	{
		/* (beyond the resolution now, that way, and nearer than any found) */
		if (direction * game_setting_resolution_compare(widths[index], heights[index], current_width, current_height) <= 0)
			continue;
		if (found && direction * game_setting_resolution_compare(widths[index], heights[index], *width, *height) >= 0)
			continue;
		*width = widths[index];
		*height = heights[index];
		found = TRUE;
	}
	return found;
}

/* steps the resolution through the display's sizes, now and in config.toml
(its width and its height); as game_setting_step */
static int game_setting_resolution_step(int direction)
{
	int width, height;
	char text[16];
	int written;

	if (!game_setting_resolution_next(direction, &width, &height))
		return 0;
	game_setting_resolution_format(width, height, text, (int)sizeof(text));
	if (!platform_set_fullscreen_resolution(width, height))
	{
		platform_fullscreen_resolution(&width, &height);
		game_setting_resolution_format(width, height, text, (int)sizeof(text));
		platform_log("settings: display.resolution stays %s", text);
		return 0;
	}
	written = config_write_integer("display.resolution_width", width);
	written = config_write_integer("display.resolution_height", height) && written;
	game_setting_log_written("display.resolution", text, written);
	return 1;
}

/* ---------- the settings */

static void game_setting_format(int setting, double value, char *text, int size)
{
	switch (game_settings[setting].type)
	{
	case _game_setting_type_switch:
		snprintf(text, (size_t)size, "%s", value != 0.0 ? "ON" : "OFF");
		break;
	case _game_setting_type_integer:
		snprintf(text, (size_t)size, "%ld", lround(value));
		break;
	case _game_setting_type_percentage:
		snprintf(text, (size_t)size, "%ld%%", lround(value * 100.0));
		break;
	case _game_setting_type_real:
		/* (two decimals only for a value between the steps) */
		snprintf(text, (size_t)size, fabs(value * 10.0 - floor(value * 10.0 + 0.5)) < 0.001 ? "%.1f" : "%.2f", value);
		break;
	case _game_setting_type_resolution:
		/* (a width and a height, not a value: game_setting_text's) */
		if (size > 0)
			text[0] = 0;
		break;
	case _game_setting_type_multiple:
		if (value > 1.0)
			snprintf(text, (size_t)size, "%ldX", lround(value));
		else
			snprintf(text, (size_t)size, "OFF");
		break;
	}
}

int game_setting_is_switch(int setting)
{
	return setting < 0 || setting >= NUMBER_OF_GAME_SETTINGS ||
		game_settings[setting].type == _game_setting_type_switch;
}

void game_setting_text(int setting, char *text, int size)
{
	if (setting < 0 || setting >= NUMBER_OF_GAME_SETTINGS)
	{
		if (size > 0)
			text[0] = 0;
		return;
	}
	if (game_settings[setting].type == _game_setting_type_resolution)
	{
		int width, height;

		platform_fullscreen_resolution(&width, &height);
		game_setting_resolution_format(width, height, text, size);
		return;
	}
	game_setting_format(setting, game_setting_value(setting), text, size);
}

int game_setting_step(int setting, int direction)
{
	const struct game_setting *definition;
	double value, next;
	char text[16];
	int written;

	if (setting < 0 || setting >= NUMBER_OF_GAME_SETTINGS)
		return 0;
	definition = &game_settings[setting];
	if (definition->type == _game_setting_type_resolution)
		return game_setting_resolution_step(direction);
	value = game_setting_value(setting);
	if (definition->type == _game_setting_type_switch)
	{
		next = value == 0.0;
	}
	else
	{
		double maximum = game_setting_maximum(setting);

		if (definition->type == _game_setting_type_multiple)
		{
			/* (the power of two past the value that way: a value between two
			joins them) */
			next = 1.0;
			while (direction > 0 ? next <= value + 0.001 : next * 2.0 < value - 0.001)
				next *= 2.0;
			if (direction < 0 && next >= value - 0.001)
				next /= 2.0;
		}
		else
		{
			double steps = direction > 0 ? floor(value / definition->step + 0.001) + 1.0 :
				ceil(value / definition->step - 0.001) - 1.0;

			next = steps * definition->step;
		}
		/* (from beyond the range, only back toward it) */
		if (next < definition->minimum - 0.001 || (direction > 0 && next > maximum + 0.001))
			return 0;
	}
	game_setting_format(setting, next, text, (int)sizeof(text));
	if (!game_setting_apply(setting, next))
	{
		game_setting_format(setting, game_setting_value(setting), text, (int)sizeof(text));
		platform_log("settings: %s stays %s", definition->name, text);
		return 0;
	}
	switch (definition->type)
	{
	case _game_setting_type_switch:
		written = config_write_boolean(definition->name, next != 0.0);
		break;
	case _game_setting_type_integer:
	case _game_setting_type_multiple:
		written = config_write_integer(definition->name, lround(next));
		break;
	default:
		written = config_write_real(definition->name, next);
		break;
	}
	game_setting_log_written(definition->name, text, written);
	return 1;
}

#endif
