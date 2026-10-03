/*
MENU_FUNCTIONS.C

The event handler functions the menus written in XML can run besides the
game's (menu_tags.c names them; ui_widget_event_handler_function_invoke
calls them from PC_MENU_FUNCTION_BASE on):

- "port quit game" (and the PC version's "main menu quit game") quits, as
  closing the window does;
- the PC version's "profile set edit begin" begins editing the first player
  profile, as its settings screens need, and fails with none (its handlers
  then open the screens that make one);
- "gamespy screen init" hides the server browser's error and filter panels,
  and the title of the mode "mp type set mode" did not choose (Internet or
  LAN);
- its buttons' events: "mouse emit back event" and "mouse emit x event" push
  B and X (as the mouse does), "emit custom activation event" (an OK
  button) runs the screen's custom activation handler, "single prev cl item
  activated" (an item chosen) its list's, and its back handlers go back;
- "port setting load", on a spinner's created event, shows the value its
  setting has (or the nearest of its values): config.toml's, or for
  "profile.<field>" the controller's in the profile being edited;
- "port setting save", on its deleted event, writes the value shown back,
  if the player changed it;
- the settings screens' (tools/port_settings.py): "port settings save"
  (OK) writes those changed and applies the window's (the rest follow
  config.toml themselves), "port settings defaults" shows the defaults,
  "port settings help" the help of the row chosen;
- Controls Setup's: the keyboard and mouse's controls, two bindings each,
  shown a group at a time ("controls update menu"); "controls begin
  binding" takes the next key or button pressed for the binding left and
  right choose ("controls binding slot"); OK ("controls screen change set")
  writes them, "controls screen defaults" shows the defaults.

A spinner's values are its strings' (setting= and values= in its <widget>,
menu_tags.c's pc_menu_setting).

The PC version's campaign menus, on player 1's profile (the active one, else
the one last used, else the first) and the game's saved game in it:
- "campaign menu continue" goes on with the saved game;
- New Game's list of levels ("initialize sp level list solo", "solo map
  list update", "solo level set map") has those the profile has reached, and
  marks those it has finished on Normal, Heroic and Legendary, as the
  Xbox's list does; a level with the saved game in it goes on with it, at
  its difficulty;
- the difficulty menu: "difficulty item select" (a difficulty chosen) and
  "set difficulty" (its OK button: the difficulty shown) start the game;
- Load Game's list ("load game menu init", "load game list update", "load
  game menu activated") has each profile's saved game, and "load game menu
  delete request" and "delete finish" delete one.
The Xbox's functions of those names take the Xbox's widgets (a spinner of
levels, the difficulty list itself), so ours run instead (menu_tags.c).

The rest are the PC version's, which its menus (port/assets/menus/ce)
name and the Xbox's has not: they do nothing yet, and succeed, so that what
their handlers open opens.
*/

#include "cseries.h"
#include "input/input.h"
#include "interface/event_manager.h"
#include "interface/player_ui.h"
#include "interface/ui_widget.h"
#include "main/main.h"
#include "networking/network_game_manager.h"
#include "saved games/player_profile.h"
#include "tag_files/tag_groups.h"
#include "text/text_group.h"
#include "text/unicode.h"

#include "halo_menus.h"
#include "halo_custom_maps.h"

#include <stdlib.h>
#include <string.h>
#include <xtl.h>

/* the platform layer's (port/linux/src) */
void platform_log(char const *format, ...);
void platform_request_quit(void);
char const *pc_menu_function_name(long function_index);
char const *pc_menu_game_data_input_name(long function_index);
void event_manager_post_button(short controller_index, short button_index);
int config_text(char const *name, char *text, unsigned int size);
int config_write(char const *name, char const *value);
int config_default(char const *name, char *text, unsigned int size);
char const *config_string(char const *name);
void platform_display_apply(void);
void platform_binding_capture_begin(void);
int platform_binding_capture_poll(int *input);
void halo_input_name(int input, char *name, unsigned int size);
short pc_menu_string_index(long definition_index);

/* the game's (port) */
boolean ui_widget_port_dispatch_event(struct widget_instance *widget, short event_type, short controller_index,
	boolean *deleted);
void ui_widget_port_go_back(struct widget_instance *widget);
short ui_widget_port_list_index(struct widget_instance *list_widget);
boolean ui_widget_port_saved_game(char const **map_name, short *level, short *difficulty);
short main_get_solo_level_from_name(char const *name);

boolean pc_menu_event_function_invoke(struct widget_instance *widget, struct event_record *event,
	long function_index, boolean *widget_deleted);
void pc_menu_game_data_function_invoke(struct widget_instance *widget, long function);

/* ---------- constants */

#define MAXIMUM_STRINGS 64
/* the PC version's custom activation event (this engine never sends it) */
#define EVENT_CUSTOM_ACTIVATION 32
#define BUTTON_A 0
#define BUTTON_B 1
#define BUTTON_X 2

enum
{
	_pc_menu_function_quit_game,
	_pc_menu_function_setting_load,
	_pc_menu_function_setting_save,
	NUMBER_OF_PC_MENU_FUNCTIONS
};

/* ---------- structures */

/* a widget, as ui_widget.c has it */
struct widget_instance
{
	long definition_tag_index;
	char const *name;
	short local_player_index;
	short horizontal_offset;
	short vertical_offset;
	short type;
	boolean visible;
	boolean render_regardless_of_controller_index;
	boolean disabled;
	boolean pause_game_time;
	boolean delete_recursion_lock;
	boolean widget_is_error_dialog;
	boolean close_if_local_player_controller_present;
	byte pad17;
	long creation_time;
	unsigned long milliseconds_to_auto_close;
	unsigned long auto_close_fade_time;
	real alpha_modifier;
	struct widget_instance *previous;
	struct widget_instance *next;
	struct widget_instance *parent;
	struct widget_instance *child;
	struct widget_instance *focused_child;
	union
	{
		struct
		{
			wchar_t *text;
			short string_list_index;
		} text_box;
		struct
		{
			short selected_index;
			short last_list_tab_direction;
			void *list_items;
			word number_of_items;
			struct widget_instance *extended_description;
			wchar_t *item_text;
		} list;
	} parameters;
	struct
	{
		short current_frame_index;
		short first_frame_index;
		short last_frame_index;
		short number_of_sprite_frames;
	} animation;
};

typedef char verify_widget_instance_size[
	sizeof(struct widget_instance) == 0x58 ? 1 : -1];
typedef char verify_widget_instance_child_offset[
	offsetof(struct widget_instance, child) == 0x34 ? 1 : -1];
typedef char verify_widget_instance_parent_offset[
	offsetof(struct widget_instance, parent) == 0x30 ? 1 : -1];
typedef char verify_widget_instance_selected_index_offset[
	offsetof(struct widget_instance, parameters.list.selected_index) == 0x3C ? 1 : -1];
typedef char verify_widget_instance_number_of_items_offset[
	offsetof(struct widget_instance, parameters.list.number_of_items) == 0x44 ? 1 : -1];
typedef char verify_widget_instance_animation_offset[
	offsetof(struct widget_instance, animation) == 0x50 ? 1 : -1];

/* menu_tags.c's */
struct pc_menu_setting
{
	long definition_index;
	char const *setting;
	long value_count;
	char const *values[MAXIMUM_STRINGS];
	short loaded_index;
};

struct pc_menu_setting *pc_menu_setting_get(long definition_index);

/* ---------- private code */

static boolean campaign_profile(short controller, struct player_profile *profile);

static boolean text_is_number(char const *text, double *number)
{
	char *end;

	*number = strtod(text, &end);
	return *text && !*end;
}

/* whether the widget is one of a spinner's items, which the game makes of
its strings from its own definition, handlers and all (ui_widget.c,
ui_widget_load_children_recursive) */
static boolean spinner_item(struct widget_instance const *widget)
{
	return widget->parent && widget->parent->definition_tag_index == widget->definition_tag_index;
}

/* the value shown for the setting's: the same one, else the nearest number */
static short setting_value_index(struct pc_menu_setting const *setting, char const *current)
{
	double current_number, value_number, distance = 0.0;
	short index, nearest = 0;

	for (index = 0; index < setting->value_count; index++)
	{
		if (!_stricmp(setting->values[index], current))
			return index;
	}
	if (!text_is_number(current, &current_number))
		return 0;
	for (index = 0; index < setting->value_count; index++)
	{
		if (text_is_number(setting->values[index], &value_number))
		{
			double gap = value_number > current_number ? value_number - current_number : current_number - value_number;

			if (index == 0 || gap < distance)
			{
				distance = gap;
				nearest = index;
			}
		}
	}
	return nearest;
}

/* the profile's settings ("profile.<field>"): those of the controller in
the profile being edited */
static struct
{
	char const *name;
	short field;
	/* its default (player_profile.c's new profile's) */
	byte default_value;
} const profile_settings[] =
{
	{ "profile.look_sensitivity", 0, 3 },
	{ "profile.invert_look", 1, FALSE },
	{ "profile.flight_inversion", 2, FALSE },
	{ "profile.autocenter", 3, FALSE },
	{ "profile.button_preset", 4, 0 },
	{ "profile.joystick_preset", 5, 0 },
	{ "profile.vibration", 6, TRUE },
	{ "profile.ingame_help", 7, TRUE },
};

static byte *profile_setting_field(struct player_profile *profile, short field)
{
	struct player_profile_controller_settings *controls = &profile->controller_settings;

	switch (field)
	{
	case 0: return &controls->look_sensitivity;
	case 1: return (byte *)&controls->invert_look;
	case 2: return (byte *)&controls->flight_stick_aircraft_controls;
	case 3: return (byte *)&controls->autocenter;
	case 4: return &controls->button_preset;
	case 5: return &controls->joystick_preset;
	case 6: return (byte *)&controls->vibration_disabled;
	case 7: return (byte *)&controls->ingame_help_disabled;
	}
	return NULL;
}

static boolean profile_setting_boolean(short field)
{
	return field != 0 && field != 4 && field != 5;
}

/* a setting's value as text, or (default_value) its default: config.toml's,
or the profile's */
static boolean setting_text(char const *name, char *text, unsigned int size, boolean default_value)
{
	short index;

	if (strncmp(name, "profile.", 8))
	{
		if (!(default_value ? config_default(name, text, size) : config_text(name, text, size)))
			return FALSE;
		/* (display.mode empty: display.fullscreen's, as the window has it:
		sdl_platform.c) */
		if (!strcmp(name, "display.mode") && !text[0])
			snprintf(text, size, "%s", default_value || config_boolean("display.fullscreen") ? "borderless" : "windowed");
		return TRUE;
	}
	for (index = 0; index < NUMBEROF(profile_settings); index++)
	{
		struct player_profile *profile = player_ui_get_edit_player_profile();
		short field = profile_settings[index].field;
		long value;

		if (strcmp(name, profile_settings[index].name))
			continue;
		if (default_value)
			value = profile_settings[index].default_value;
		else if (profile && profile_setting_field(profile, field))
			value = *profile_setting_field(profile, field);
		else
			return FALSE;
		/* (vibration and help are kept as their opposites) */
		if (!default_value && (field == 6 || field == 7))
			value = !value;
		if (profile_setting_boolean(field))
			snprintf(text, size, "%s", value ? "true" : "false");
		else
			snprintf(text, size, "%ld", value);
		return TRUE;
	}
	return FALSE;
}

static boolean setting_write(char const *name, char const *value)
{
	short index;

	if (strncmp(name, "profile.", 8))
		return config_write(name, value);
	for (index = 0; index < NUMBEROF(profile_settings); index++)
	{
		struct player_profile *profile = player_ui_get_edit_player_profile();
		short field = profile_settings[index].field;
		byte *place;
		long number;

		if (strcmp(name, profile_settings[index].name))
			continue;
		if (!profile || !(place = profile_setting_field(profile, field)))
			return FALSE;
		number = profile_setting_boolean(field) ? !strcmp(value, "true") : atol(value);
		if (field == 6 || field == 7)
			number = !number;
		*place = (byte)number;
		return TRUE;
	}
	return FALSE;
}

static boolean setting_load(struct widget_instance *widget)
{
	struct pc_menu_setting *setting = pc_menu_setting_get(widget->definition_tag_index);
	char current[300];

	if (spinner_item(widget))
		return TRUE;
	if (!setting || !setting_text(setting->setting, current, sizeof(current), FALSE))
		return FALSE;
	setting->loaded_index = setting_value_index(setting, current);
	if (setting->loaded_index < (short)widget->parameters.list.number_of_items)
		widget->parameters.list.selected_index = setting->loaded_index;
	return TRUE;
}

static boolean setting_save(struct widget_instance *widget)
{
	struct pc_menu_setting *setting = pc_menu_setting_get(widget->definition_tag_index);

	if (spinner_item(widget))
		return TRUE;
	if (!setting || setting->loaded_index == NONE || widget->parameters.list.selected_index < 0 ||
		widget->parameters.list.selected_index >= setting->value_count)
		return FALSE;
	if (widget->parameters.list.selected_index == setting->loaded_index)
		return TRUE;
	if (!config_write(setting->setting, setting->values[widget->parameters.list.selected_index]))
	{
		platform_log("menus: could not write %s to config.toml", setting->setting);
		return FALSE;
	}
	platform_log("menus: %s = %s (from the next start)", setting->setting, setting->values[widget->parameters.list.selected_index]);
	setting->loaded_index = widget->parameters.list.selected_index;
	return TRUE;
}

static short controller_of(struct widget_instance const *widget)
{
	return widget->local_player_index >= 0 && widget->local_player_index < 4 ? widget->local_player_index : 0;
}

/* the controller the event came from, else the widget's */
static short event_controller(struct widget_instance const *widget, struct event_record const *event)
{
	return event && event->controller_index >= 0 && event->controller_index < 4 ?
		event->controller_index : controller_of(widget);
}

/* runs the custom activation handler of the widget's list, or the nearest
of its parents' that has one (widget_deleted: the widget went with its
screen) */
static boolean item_activated(struct widget_instance *widget, short controller, boolean *widget_deleted)
{
	struct widget_instance *parent;

	for (parent = widget->parent; parent; parent = parent->parent)
	{
		boolean deleted;

		if (ui_widget_port_dispatch_event(parent, EVENT_CUSTOM_ACTIVATION, controller, &deleted))
		{
			if (deleted)
				*widget_deleted = TRUE;
			return TRUE;
		}
	}
	return FALSE;
}

/* whether the multiplayer menu's choice was LAN, not Internet (mp type set
mode) */
static boolean lan_mode;

/* hides the descendants of the widget with the name */
static void hide_named(struct widget_instance *widget, char const *name)
{
	struct widget_instance *child;

	for (child = widget->child; child; child = child->next)
	{
		if (!strcmp(child->name, name))
			child->visible = FALSE;
		hide_named(child, name);
	}
}

/* the server browser's panels for errors and filters are hidden until they
are wanted, and its title is Internet's or LAN's (gamespy screen init) */
static void server_browser_initialize(struct widget_instance *screen)
{
	struct widget_instance *child;

	hide_named(screen, lan_mode ? "header_internet" : "header_lan");
	/* (a widget's name is its definition's: the last part of ours) */
	for (child = screen->child; child; child = child->next)
	{
		if (!strcmp(child->name, "gamespy_error_fullscreen") || !strcmp(child->name, "filters_screen"))
		{
			child->visible = FALSE;
			if (screen->focused_child == child)
				screen->focused_child = NULL;
		}
	}
	for (child = screen->child; child && !screen->focused_child; child = child->next)
	{
		if (child->visible && child->type == 3)
			screen->focused_child = child;
	}
}

/* begins editing player 1's profile (as the campaign has it); FALSE if
there is none */
boolean pc_menu_profile_edit_begin(void)
{
	struct player_profile profile;

	if (!campaign_profile(0, &profile))
		return FALSE;
	player_ui_begin_editing_profile(player_ui_get_active_player_profile_index(0));
	return TRUE;
}

/* ---------- the campaign */

/* the rows of its lists (main_menu/new_select/list_item_0 to 10) */
#define MAXIMUM_ROWS 11
/* its strings' and sp_levels' entries past the levels': a level not
reached, and (map_data) a saved game on the level */
#define LEVEL_UNAVAILABLE 10
#define LEVEL_IN_PROGRESS 11
#define ROW_TEXT_LENGTH 64
#define MAXIMUM_PROFILES 32
#define SOUND_ERROR 4
#define SOUND_FORWARD 2

/* (an index whose profile reads: saved_game_files.c's) */
#define PROFILE_VALID_BIT 0x80000000UL

struct campaign_level
{
	boolean available;
	/* finished on Normal, Heroic, Legendary (difficulty_options_small's
	frames 1 to 3) */
	boolean finished[3];
};

struct campaign_saved_game
{
	long profile_index;
	wchar_t profile_name[MAXIMUM_PLAYER_PROFILE_NAME_LENGTH + 1];
	short level;
	short difficulty;
};

static struct
{
	struct campaign_level levels[NUMBER_OF_SINGLE_PLAYER_LEVELS];
	/* player 1's saved game's level, else NONE */
	short saved_level;
	/* the level and saved game the lists' descriptions last showed */
	short shown_level;
	short shown_saved_game;
	/* Load Game's */
	struct campaign_saved_game saved_games[MAXIMUM_ROWS];
	short saved_game_count;
	short saved_game_to_delete;
} campaign;

/* the descendant of the widget with the name (a widget's name is its
definition's: the last part of ours), the nth of them */
static struct widget_instance *descendant(struct widget_instance *widget, char const *name, long *nth)
{
	struct widget_instance *child;

	for (child = widget->child; child; child = child->next)
	{
		struct widget_instance *found;

		if (!strcmp(child->name, name) && (*nth)-- == 0)
			return child;
		found = descendant(child, name, nth);
		if (found)
			return found;
	}
	return NULL;
}

static struct widget_instance *named(struct widget_instance *widget, char const *name, long nth)
{
	return widget ? descendant(widget, name, &nth) : NULL;
}

/* the text box's own text (as the server list sets its items'), in a
buffer of length characters (made the first time: a text box is always
given the same length) */
static void text_set_length(struct widget_instance *text_box, wchar_t const *text, short length)
{
	if (!text_box)
		return;
	if (!text_box->parameters.text_box.text)
	{
		text_box->parameters.text_box.text = ui_widget_realloc(NULL, length * sizeof(wchar_t),
			__FILE__, __LINE__);
	}
	if (text_box->parameters.text_box.text)
	{
		ustrncpy(text_box->parameters.text_box.text, text, length - 1);
		text_box->parameters.text_box.text[length - 1] = 0;
	}
}

static void text_set(struct widget_instance *text_box, wchar_t const *text)
{
	text_set_length(text_box, text, ROW_TEXT_LENGTH);
}

/* one of our string lists' strings, on one line (its line breaks spaces) */
static void string_get(char const *list, short index, wchar_t *text)
{
	long tag_index = tag_loaded('ustr', list);
	wchar_t const *string = tag_index != NONE ? unicode_string_list_get_string(tag_index, index) : NULL;
	long length = 0;

	for (; string && *string && length < ROW_TEXT_LENGTH - 1; string++)
	{
		wchar_t character = *string == '\r' || *string == '\n' ? ' ' : *string;

		if (character != ' ' || (length && text[length - 1] != ' '))
			text[length++] = character;
	}
	while (length && text[length - 1] == ' ')
		length--;
	text[length] = 0;
}

/* the list's row that has the focus, else NONE (its buttons) */
static short focused_row(struct widget_instance *list)
{
	struct widget_instance *child;
	short index = 0;

	for (child = list->child; child && index < MAXIMUM_ROWS; child = child->next, index++)
	{
		if (child == list->focused_child)
			return strncmp(child->name, "list_item_", 10) ? NONE : index;
	}
	return NONE;
}

static void focus_row(struct widget_instance *list, short row)
{
	struct widget_instance *child = list->child;
	short index;

	for (index = 0; child && child->next && index < row; index++)
		child = child->next;
	if (row == NONE)
	{
		/* (its buttons: the last child) */
		while (child && child->next)
			child = child->next;
	}
	if (child)
	{
		list->focused_child = child;
		list->parameters.list.selected_index = row == NONE ? 0 : row;
	}
}

/* shows the first count rows, with their texts, the arrows on the one that
has the focus, and no scroll buttons (the rows are enough) */
static void rows_update(struct widget_instance *list, short count, void (*row_text)(short row, wchar_t *text))
{
	struct widget_instance *row;
	short index = 0;

	for (row = list->child; row && index < MAXIMUM_ROWS; row = row->next, index++)
	{
		wchar_t text[ROW_TEXT_LENGTH];
		struct widget_instance *arrows = named(row, "list_item_arrows", 0);

		if (strncmp(row->name, "list_item_", 10))
			break;
		row->visible = index < count;
		if (index < count)
		{
			row_text(index, text);
			text_set(named(row, "list_item_text", 0), text);
		}
		if (arrows)
			arrows->visible = row == list->focused_child;
		if (named(row, "scroll_up_button", 0))
			named(row, "scroll_up_button", 0)->visible = FALSE;
		if (named(row, "scroll_down_button", 0))
			named(row, "scroll_down_button", 0)->visible = FALSE;
	}
}

/* player 1's profile, read again (the active one, else the one last used,
else the first), on the controller; FALSE if there is none */
static boolean campaign_profile(short controller, struct player_profile *profile)
{
	long profile_index = player_ui_get_active_player_profile_index(0);

	if (profile_index == NONE || !player_profile_get(profile_index, profile))
	{
		profile_index = player_ui_get_player1_last_used_profile_index();
		if (profile_index == NONE || !player_profile_get(profile_index, profile))
		{
			word count = 1;

			profile_index = NONE;
			player_profiles_enumerate_available_to_local_player_index(NONE, &count, &profile_index, FALSE);
			if ((short)count <= 0 || profile_index == NONE || !player_profile_get(profile_index, profile))
				return FALSE;
		}
	}
	player_ui_set_active_player_profile(0, profile_index, profile);
	player_ui_set_single_player_local_player_controller(0, controller);
	return TRUE;
}

/* the levels the profile has reached (as the Xbox's list has them: those
it has played, the one after the last it finished, the first) and finished,
and its saved game's */
static void campaign_levels_read(struct player_profile const *profile)
{
	char const *map_name;
	short highest_level, highest_difficulty, difficulty, level;

	player_profile_get_highest_completed_solo_level((struct player_profile *)profile, &highest_level,
		&highest_difficulty);
	for (level = 0; level < NUMBER_OF_SINGLE_PLAYER_LEVELS; level++)
	{
		byte flags = profile->single_player_map_flags[level];
		short marker;

		campaign.levels[level].available = flags || level == highest_level + 1 || level == 0;
		for (marker = 0; marker < 3; marker++)
			campaign.levels[level].finished[marker] = (flags >> (marker + 1)) & 1;
	}
	if (!ui_widget_port_saved_game(&map_name, &campaign.saved_level, &difficulty))
		campaign.saved_level = NONE;
	(void)map_name;
}

/* plays the map, at the difficulty (a saved game in it goes on: main.c's
main_new_map, if its difficulty is this one) */
static void campaign_start(char const *map_name, short difficulty, short controller)
{
	player_ui_set_single_player_local_player_controller(0, controller);
	main_set_map_name(map_name);
	main_set_difficulty(difficulty);
	game_connection_set(0);
	main_menu_switch_to_single_player();
	player_ui_remember_player1_profile(TRUE);
	ui_play_audio_feedback_sound(SOUND_FORWARD);
}

static boolean campaign_fail(void)
{
	ui_play_audio_feedback_sound(SOUND_ERROR);
	return FALSE;
}

/* the description's level: its name, picture and words, and the
difficulties it has been finished on (finished: New Game's, NULL for a level
not reached), or (saved_difficulty, not NONE: Load Game's) the saved game's
difficulty */
static void level_description(struct widget_instance *description, char const *prefix, short level, boolean in_progress,
	boolean const *finished, short saved_difficulty)
{
	char name[64];
	struct widget_instance *widget;
	short marker;

	snprintf(name, sizeof(name), "%s_right_name", prefix);
	if ((widget = named(description, name, 0)) != NULL)
		widget->parameters.text_box.string_list_index = level;
	snprintf(name, sizeof(name), "%s_right_pic", prefix);
	if ((widget = named(description, name, 0)) != NULL)
		widget->animation.current_frame_index = level;
	snprintf(name, sizeof(name), "%s_right_data", prefix);
	if ((widget = named(description, name, 0)) != NULL && saved_difficulty != NONE)
	{
		wchar_t text[ROW_TEXT_LENGTH];

		string_get("pc\\main_menu\\player_profiles_select\\difficulty_names", saved_difficulty, text);
		text_set(widget, text);
	}
	else if (widget)
		widget->parameters.text_box.string_list_index = in_progress ? LEVEL_IN_PROGRESS : level;
	for (marker = 0; marker < 3; marker++)
	{
		if ((widget = named(description, "difficulty_indicator", marker)) != NULL)
		{
			widget->visible = saved_difficulty != NONE ? marker == 0 : finished && finished[marker];
			widget->animation.current_frame_index = saved_difficulty != NONE ? saved_difficulty : marker + 1;
		}
	}
}

/* player 1's profile's name (the active one, else the one used last: the
main menu clears the active ones), on the descriptions'
current_profile_name */
static void profile_name_show(struct widget_instance *description)
{
	/* (the profile read from its file at most once a second) */
	static struct player_profile profile;
	static long read_index = NONE;
	static unsigned long read_time;
	long index = player_ui_get_active_player_profile_index(0);

	if (!description)
		return;
	if (index != NONE)
	{
		player_ui_get_active_player_profile(0, &profile);
		read_index = NONE;
	}
	else if ((index = player_ui_get_player1_last_used_profile_index()) == NONE)
		return;
	else if (index != read_index || system_milliseconds() - read_time > 1000)
	{
		read_index = NONE;
		if (!player_profile_get(index, &profile))
			return;
		read_index = index;
		read_time = system_milliseconds();
	}
	text_set(named(description, "current_profile_name", 0), profile.player_name);
}

/* (Campaign's menu) "campaign menu init": player 1's profile and saved
game, for its items */
static boolean campaign_menu_initialize(short controller)
{
	struct player_profile profile;

	if (campaign_profile(controller, &profile))
		campaign_levels_read(&profile);
	return TRUE;
}

/* "campaign menu continue": the saved game goes on */
static boolean campaign_continue(short controller)
{
	struct player_profile profile;
	char const *map_name;
	short level, difficulty;

	if (!campaign_profile(controller, &profile) || !ui_widget_port_saved_game(&map_name, &level, &difficulty))
		return campaign_fail();
	campaign_start(map_name, difficulty, controller);
	return TRUE;
}

/* New Game's list: "initialize sp level list solo" starts on the level last
played */
static boolean level_list_initialize(struct widget_instance *list, short controller)
{
	struct player_profile profile;
	short level;

	if (!campaign_profile(controller, &profile))
		return FALSE;
	campaign_levels_read(&profile);
	level = PIN(profile.last_single_player_map_played, 0, NUMBER_OF_SINGLE_PLAYER_LEVELS - 1);
	if (campaign.saved_level != NONE)
		level = campaign.saved_level;
	if (!campaign.levels[level].available)
		level = 0;
	campaign.shown_level = level;
	focus_row(list, level);
	return TRUE;
}

static void level_row_text(short row, wchar_t *text)
{
	string_get("pc\\main_menu\\map_list", campaign.levels[row].available ? row : LEVEL_UNAVAILABLE, text);
}

/* "solo map list update" */
static void level_list_update(struct widget_instance *list)
{
	short row = focused_row(list), level;

	rows_update(list, NUMBER_OF_SINGLE_PLAYER_LEVELS, level_row_text);
	if (row != NONE && row < NUMBER_OF_SINGLE_PLAYER_LEVELS)
		campaign.shown_level = row;
	level = campaign.shown_level;
	if (!campaign.levels[level].available)
		level = LEVEL_UNAVAILABLE;
	level_description(list->parameters.list.extended_description, "replay_level", level,
		level == campaign.saved_level, level == LEVEL_UNAVAILABLE ? NULL : campaign.levels[level].finished, NONE);
	profile_name_show(list->parameters.list.extended_description);
}

/* "solo level set map": the level shown, if reached, for the difficulty
menu */
static boolean level_choose(void)
{
	short level = campaign.shown_level;

	if (level < 0 || level >= NUMBER_OF_SINGLE_PLAYER_LEVELS || !campaign.levels[level].available)
		return campaign_fail();
	/* (not yet: setting it at the main menu changes map, as the Xbox's has it
	not) */
	main_set_map_name(main_get_solo_level_name(level));
	main_defer_map_map_change();
	return TRUE;
}

/* the difficulty menu: "difficulty item select" starts the game at the
item's; "set difficulty" (its OK button) at the one its description shows */
static boolean difficulty_start(short difficulty, short controller)
{
	struct player_profile profile;
	char const *map_name = main_get_map_name();

	if (!campaign_profile(controller, &profile))
		return campaign_fail();
	if (!map_name || main_get_solo_level_from_name(map_name) == NONE)
		map_name = main_get_solo_level_name(0);
	campaign_start(map_name, PIN(difficulty, 0, 3), controller);
	return TRUE;
}

static short difficulty_shown(struct widget_instance *widget)
{
	for (; widget; widget = widget->parent)
	{
		if (!strcmp(widget->name, "difficulty_select_list") && widget->parameters.list.extended_description)
		{
			struct widget_instance *picture = named(widget->parameters.list.extended_description,
				"difficulty_options", 0);

			return picture ? picture->animation.current_frame_index : main_get_difficulty();
		}
	}
	return main_get_difficulty();
}

static short sibling_index(struct widget_instance *widget)
{
	struct widget_instance *child;
	short index = 0;

	for (child = widget->parent ? widget->parent->child : widget; child && child != widget; child = child->next)
		index++;
	return index;
}

/* Load Game's list: each profile's saved game (read with the profile as
player 1's, which then is again) */
static void saved_games_read(short controller)
{
	struct player_profile profile, active;
	long profiles[MAXIMUM_PROFILES];
	long active_index;
	word count = MAXIMUM_PROFILES;
	short index;

	campaign.saved_game_count = 0;
	if (!campaign_profile(controller, &active))
		return;
	active_index = player_ui_get_active_player_profile_index(0);
	player_profiles_enumerate_available_to_local_player_index(NONE, &count, profiles, FALSE);
	for (index = 0; index < (short)count && campaign.saved_game_count < MAXIMUM_ROWS; index++)
	{
		struct campaign_saved_game *saved_game = &campaign.saved_games[campaign.saved_game_count];
		char const *map_name;

		if (!((unsigned long)profiles[index] & PROFILE_VALID_BIT) || !player_profile_get(profiles[index], &profile))
			continue;
		player_ui_set_active_player_profile(0, profiles[index], &profile);
		if (ui_widget_port_saved_game(&map_name, &saved_game->level, &saved_game->difficulty))
		{
			saved_game->profile_index = profiles[index];
			ustrncpy(saved_game->profile_name, profile.player_name, MAXIMUM_PLAYER_PROFILE_NAME_LENGTH);
			saved_game->profile_name[MAXIMUM_PLAYER_PROFILE_NAME_LENGTH] = 0;
			campaign.saved_game_count++;
		}
	}
	player_ui_set_active_player_profile(0, active_index, &active);
	campaign_levels_read(&active);
}

/* "load game menu init" */
static boolean saved_game_list_initialize(struct widget_instance *list, short controller)
{
	short index;

	saved_games_read(controller);
	campaign.shown_saved_game = 0;
	for (index = 0; index < campaign.saved_game_count; index++)
	{
		if (campaign.saved_games[index].profile_index == player_ui_get_active_player_profile_index(0))
			campaign.shown_saved_game = index;
	}
	focus_row(list, campaign.saved_game_count ? campaign.shown_saved_game : NONE);
	return TRUE;
}

static void saved_game_row_text(short row, wchar_t *text)
{
	ustrncpy(text, campaign.saved_games[row].profile_name, ROW_TEXT_LENGTH - 1);
	text[ROW_TEXT_LENGTH - 1] = 0;
}

/* "load game list update" */
static void saved_game_list_update(struct widget_instance *list)
{
	struct widget_instance *description = list->parameters.list.extended_description;
	struct widget_instance *item = named(description, "load_level_right_item", 0);
	short row = focused_row(list);

	rows_update(list, campaign.saved_game_count, saved_game_row_text);
	if (row != NONE && row < campaign.saved_game_count)
		campaign.shown_saved_game = row;
	if (campaign.shown_saved_game >= campaign.saved_game_count)
		campaign.shown_saved_game = campaign.saved_game_count - 1;
	if (item)
		item->visible = campaign.saved_game_count > 0;
	if (campaign.saved_game_count > 0)
	{
		struct campaign_saved_game const *saved_game = &campaign.saved_games[campaign.shown_saved_game];

		level_description(description, "load_level", saved_game->level, FALSE, NULL, saved_game->difficulty);
	}
	else if (row != NONE)
	{
		focus_row(list, NONE);
	}
	profile_name_show(description);
}

/* "load game menu activated": the saved game goes on, with its profile as
player 1's */
static boolean saved_game_continue(short controller)
{
	struct campaign_saved_game const *saved_game;
	struct player_profile profile;

	if (campaign.shown_saved_game < 0 || campaign.shown_saved_game >= campaign.saved_game_count)
		return campaign_fail();
	saved_game = &campaign.saved_games[campaign.shown_saved_game];
	if (!player_profile_get(saved_game->profile_index, &profile))
		return campaign_fail();
	player_ui_set_active_player_profile(0, saved_game->profile_index, &profile);
	campaign_start(main_get_solo_level_name(saved_game->level), saved_game->difficulty, controller);
	return TRUE;
}

/* "load game menu delete request" (before its question) and "delete
finish" (its OK): the saved game shown goes */
static boolean saved_game_delete_request(void)
{
	if (campaign.shown_saved_game < 0 || campaign.shown_saved_game >= campaign.saved_game_count)
		return campaign_fail();
	campaign.saved_game_to_delete = campaign.shown_saved_game;
	return TRUE;
}

static boolean saved_game_delete(short controller)
{
	char path[256];

	if (campaign.saved_game_to_delete < 0 || campaign.saved_game_to_delete >= campaign.saved_game_count ||
		!player_profile_get_enclosing_directory_path(campaign.saved_games[campaign.saved_game_to_delete].profile_index,
			path) || strlen(path) + strlen("savegame.bin") >= sizeof(path))
	{
		return campaign_fail();
	}
	strcat(path, "savegame.bin");
	if (!DeleteFileA(path))
		platform_log("menus: could not delete %s", path);
	campaign.saved_game_to_delete = NONE;
	saved_games_read(controller);
	return TRUE;
}

/* the settings' screens (tools/port_settings.py): every spinner of a
setting in the screen */
static void settings_each(struct widget_instance *widget, boolean (*visit)(struct widget_instance *spinner,
	struct pc_menu_setting *setting))
{
	struct widget_instance *child;

	for (child = widget->child; child; child = child->next)
	{
		struct pc_menu_setting *setting = child->type == 2 && !spinner_item(child) ?
			pc_menu_setting_get(child->definition_tag_index) : NULL;

		if (setting)
			visit(child, setting);
		else
			settings_each(child, visit);
	}
}

static struct widget_instance *screen_of(struct widget_instance *widget)
{
	while (widget->parent)
		widget = widget->parent;
	return widget;
}

/* "port settings save" (OK): those changed written, and applied */
static boolean setting_changed_save(struct widget_instance *spinner, struct pc_menu_setting *setting)
{
	short index = spinner->parameters.list.selected_index;

	if (index < 0 || index >= setting->value_count || index == setting->loaded_index)
		return TRUE;
	if (!setting_write(setting->setting, setting->values[index]))
	{
		platform_log("menus: could not set %s", setting->setting);
		return FALSE;
	}
	platform_log("menus: %s = %s", setting->setting, setting->values[index]);
	setting->loaded_index = index;
	return TRUE;
}

/* "port settings defaults" */
static boolean setting_default_show(struct widget_instance *spinner, struct pc_menu_setting *setting)
{
	char text[300];

	if (setting_text(setting->setting, text, sizeof(text), TRUE))
		spinner->parameters.list.selected_index = setting_value_index(setting, text);
	return TRUE;
}

/* "port settings help": the line of the row chosen (by its label's string:
the buttons' is the first) */
static void settings_help(struct widget_instance *list)
{
	struct widget_instance *help = list->parameters.list.extended_description;
	struct widget_instance *row = list->focused_child;
	short index = 0;

	if (!help)
		return;
	if (row && row->child && row->child->type == 1 && strncmp(row->name, "button", 6))
	{
		short label = pc_menu_string_index(row->child->definition_tag_index);

		if (label != NONE)
			index = label + 1;
	}
	help->parameters.text_box.string_list_index = index;
}

/* ---------- Change Color: the profile's colour, from a list of the
game's colours (more than its rows: it scrolls), by the Xbox's names for
its spinner's functions */

#define COLOR_COUNT 18
#define COLOR_ROWS 11

static struct
{
	/* the colour on the first row, and the colour chosen */
	short first;
	short color;
} color_list;

static void color_row_text(short row, wchar_t *text)
{
	string_get("pc\\main_menu\\settings_select\\player_setup\\player_profile_edit\\color_edit\\colors_list",
		(short)(color_list.first + row), text);
}

/* "color picker menu initialize": on the profile's colour */
static boolean color_list_initialize(struct widget_instance *list)
{
	struct player_profile *profile = player_ui_get_edit_player_profile();

	if (!profile)
		return FALSE;
	color_list.color = (short)PIN(profile->primary_color_index, 0, COLOR_COUNT - 1);
	color_list.first = (short)PIN(color_list.color - COLOR_ROWS / 2, 0, COLOR_COUNT - COLOR_ROWS);
	focus_row(list, (short)(color_list.color - color_list.first));
	return TRUE;
}

/* a list of more items than its rows: the row chosen kept off its ends
while there are more past them, the list moving instead; the item chosen,
or NONE (its buttons) */
static short list_scroll(struct widget_instance *list, short *first, short count, short rows)
{
	short row = focused_row(list);

	if (row == rows - 1 && *first + rows < count)
	{
		(*first)++;
		focus_row(list, --row);
	}
	else if (row == 0 && *first > 0)
	{
		(*first)--;
		focus_row(list, ++row);
	}
	return row == NONE || row >= rows ? NONE : (short)(*first + row);
}

/* "color picker update": the list scrolled on at its ends, the colour's
name and picture */
static void color_list_update(struct widget_instance *list)
{
	struct widget_instance *description = list->parameters.list.extended_description;
	struct widget_instance *widget;
	short color = list_scroll(list, &color_list.first, COLOR_COUNT, COLOR_ROWS);

	if (color != NONE)
		color_list.color = color;
	rows_update(list, COLOR_ROWS, color_row_text);
	if ((widget = named(description, "color_right_name", 0)) != NULL)
		widget->parameters.text_box.string_list_index = color_list.color;
	if ((widget = named(description, "color_right_pic", 0)) != NULL)
		widget->animation.current_frame_index = color_list.color;
	profile_name_show(description);
}

/* "color picker select color" */
static boolean color_choose(void)
{
	struct player_profile *profile = player_ui_get_edit_player_profile();

	if (!profile)
		return FALSE;
	profile->primary_color_index = color_list.color;
	return TRUE;
}

/* ---------- Profiles: the player profiles, and a row to make one (as the
PC version's list has them, but by the Xbox's names for its spinner's
functions); choosing one makes it player 1's, the profile the campaign and
Settings use; deleting one asks first */

#define PROFILE_ROWS 11
#define PROFILE_DEFAULT_BIT 0x40000000UL

static struct
{
	long indices[MAXIMUM_PROFILES];
	struct player_profile profiles[MAXIMUM_PROFILES];
	/* the profiles; the row after them makes a new one */
	short count;
	short first;
	short chosen;
	long to_delete;
} profile_list;

/* the profiles, read again when they are not those read last */
static void profile_list_read(boolean always)
{
	long indices[MAXIMUM_PROFILES];
	word count = MAXIMUM_PROFILES;
	short index, valid = 0;

	player_profiles_enumerate_available_to_local_player_index(NONE, &count, indices, FALSE);
	for (index = 0; index < (short)count; index++)
	{
		if ((unsigned long)indices[index] & PROFILE_VALID_BIT)
			indices[valid++] = indices[index];
	}
	if (!always && valid == profile_list.count &&
		!memcmp(indices, profile_list.indices, valid * sizeof(indices[0])))
	{
		return;
	}
	profile_list.count = 0;
	for (index = 0; index < valid; index++)
	{
		if (player_profile_get(indices[index], &profile_list.profiles[profile_list.count]))
			profile_list.indices[profile_list.count++] = indices[index];
	}
	profile_list.chosen = (short)PIN(profile_list.chosen, 0, profile_list.count);
}

static short profile_list_rows(void)
{
	return (short)MIN(profile_list.count + 1, PROFILE_ROWS);
}

/* "player profile list initialize": on player 1's profile */
static boolean profile_list_initialize(struct widget_instance *list)
{
	long active = player_ui_get_active_player_profile_index(0);
	short index;

	profile_list_read(TRUE);
	if (active == NONE)
		active = player_ui_get_player1_last_used_profile_index();
	profile_list.chosen = 0;
	for (index = 0; index < profile_list.count; index++)
	{
		if (profile_list.indices[index] == active)
			profile_list.chosen = index;
	}
	profile_list.first = (short)PIN(profile_list.chosen - PROFILE_ROWS / 2, 0,
		MAX(0, profile_list.count + 1 - PROFILE_ROWS));
	focus_row(list, (short)(profile_list.chosen - profile_list.first));
	return TRUE;
}

static void profile_row_text(short row, wchar_t *text)
{
	short item = (short)(profile_list.first + row);

	if (item < profile_list.count)
	{
		ustrncpy(text, profile_list.profiles[item].player_name, MAXIMUM_PLAYER_PROFILE_NAME_LENGTH);
		text[MAXIMUM_PLAYER_PROFILE_NAME_LENGTH] = 0;
	}
	else
	{
		string_get("pc\\main_menu\\player_profiles_select\\profile_description_labels", 4, text);
	}
}

static void visible_set(struct widget_instance *widget, boolean visible)
{
	if (widget)
		widget->visible = visible;
}

/* "3wide player profile list update": the rows, and the profile chosen:
its name, colour, current level, best difficulty and controls */
static void profile_list_update(struct widget_instance *list)
{
	struct widget_instance *description = list->parameters.list.extended_description;
	struct widget_instance *widget;
	short item;
	boolean profile;

	profile_list_read(FALSE);
	item = list_scroll(list, &profile_list.first, (short)(profile_list.count + 1), profile_list_rows());
	if (item != NONE)
		profile_list.chosen = item;
	rows_update(list, profile_list_rows(), profile_row_text);
	profile = profile_list.chosen < profile_list.count;
	text_set(named(description, "player_profile_right_name", 0),
		profile ? profile_list.profiles[profile_list.chosen].player_name : L"");
	visible_set(named(description, "empty_profile_slot_text", 0), !profile);
	visible_set(named(description, "qtr_screen_profile_color_pic", 0), profile);
	visible_set(named(description, "current_level_label", 0), profile);
	visible_set(named(description, "current_level", 0), profile);
	visible_set(named(description, "joystick_controls_label", 0), profile);
	visible_set(named(description, "joystick_controls", 0), profile);
	visible_set(named(description, "skill_level_label", 0), FALSE);
	visible_set(named(description, "skill_level", 0), FALSE);
	if (profile)
	{
		struct player_profile *chosen = &profile_list.profiles[profile_list.chosen];
		short level, difficulty;

		if ((widget = named(description, "qtr_screen_profile_color_pic", 0)) != NULL)
			widget->animation.current_frame_index = (short)PIN(chosen->primary_color_index, 0, COLOR_COUNT - 1);
		if ((widget = named(description, "current_level", 0)) != NULL)
		{
			widget->parameters.text_box.string_list_index =
				(short)PIN(chosen->last_single_player_map_played, 0, NUMBER_OF_SINGLE_PLAYER_LEVELS - 1);
		}
		if ((widget = named(description, "joystick_controls", 0)) != NULL)
			widget->parameters.text_box.string_list_index = chosen->controller_settings.invert_look ? 1 : 0;
		player_profile_get_highest_completed_solo_level(chosen, &level, &difficulty);
		if (level != NONE && (widget = named(description, "skill_level", 0)) != NULL)
		{
			widget->parameters.text_box.string_list_index = difficulty;
			widget->visible = TRUE;
			visible_set(named(description, "skill_level_label", 0), TRUE);
		}
	}
	profile_name_show(description);
}

/* "profile manager select": the profile chosen made player 1's (and the
one used last); FALSE for the row that makes one (its handler then asks) */
static boolean profile_choose(short controller)
{
	struct player_profile profile;

	if (profile_list.chosen >= profile_list.count ||
		!player_profile_get(profile_list.indices[profile_list.chosen], &profile))
	{
		return FALSE;
	}
	player_ui_set_active_player_profile(0, profile_list.indices[profile_list.chosen], &profile);
	player_ui_set_single_player_local_player_controller(0, controller);
	player_ui_remember_player1_profile(TRUE);
	return TRUE;
}

/* "request del player profile" (before its question) */
static boolean profile_delete_request(void)
{
	if (profile_list.chosen >= profile_list.count)
		return campaign_fail();
	profile_list.to_delete = profile_list.indices[profile_list.chosen];
	return TRUE;
}

/* "final del player profile" (its OK): deleted, and if it was player 1's,
the first left is */
static boolean profile_delete(void)
{
	long index = profile_list.to_delete;
	boolean active = index == player_ui_get_active_player_profile_index(0);

	profile_list.to_delete = NONE;
	if (index == NONE || ((unsigned long)index & PROFILE_DEFAULT_BIT))
		return campaign_fail();
	player_profile_delete(index);
	profile_list_read(TRUE);
	if (active)
	{
		struct player_profile profile;

		if (profile_list.count && player_profile_get(profile_list.indices[0], &profile))
		{
			player_ui_set_active_player_profile(0, profile_list.indices[0], &profile);
			player_ui_remember_player1_profile(TRUE);
		}
		else
		{
			memset(&profile, 0, sizeof(profile));
			player_ui_set_active_player_profile(0, NONE, &profile);
		}
	}
	return TRUE;
}

/* ---------- Controls Setup: the keyboard and mouse's controls
(port/linux/include/halo_keyboard.h), shown a group at a time, two
bindings each */

#define CONTROL_BINDINGS 2
#define CONTROL_ROWS 7
#define CONTROL_NAME_LENGTH 32

static struct
{
	char const *setting;
	wchar_t const *label;
	short group;
} const controls[] =
{
	{ "controls.move_forward", L"MOVE FORWARD", 0 },
	{ "controls.move_backward", L"MOVE BACKWARD", 0 },
	{ "controls.strafe_left", L"STRAFE LEFT", 0 },
	{ "controls.strafe_right", L"STRAFE RIGHT", 0 },
	{ "controls.jump", L"JUMP", 0 },
	{ "controls.crouch", L"CROUCH", 0 },
	{ "controls.fire", L"FIRE", 1 },
	{ "controls.throw_grenade", L"THROW GRENADE", 1 },
	{ "controls.melee", L"MELEE", 1 },
	{ "controls.reload", L"RELOAD", 1 },
	{ "controls.zoom", L"ZOOM", 1 },
	{ "controls.switch_weapon", L"SWITCH WEAPON", 1 },
	{ "controls.switch_grenade", L"SWITCH GRENADE", 1 },
	{ "controls.action", L"ACTION", 2 },
	{ "controls.flashlight", L"FLASHLIGHT", 2 },
	{ "controls.scoreboard", L"SHOW SCORES", 2 },
	{ "controls.pause", L"PAUSE MENU", 2 },
};

static struct
{
	/* the bindings shown, until OK writes them */
	char bindings[NUMBEROF(controls)][CONTROL_BINDINGS][CONTROL_NAME_LENGTH];
	/* the binding left and right choose, and the one being set */
	short slot;
	short capturing_control;
	short capturing_slot;
} controls_screen = { { { { 0 } } }, 0, NONE, 0 };

/* a setting's bindings ("Left Ctrl, C") into the control's */
static void control_bindings_read(short control, char const *text)
{
	short slot;

	for (slot = 0; slot < CONTROL_BINDINGS; slot++)
	{
		char *binding = controls_screen.bindings[control][slot];
		unsigned int length;

		while (*text == ' ' || *text == ',')
			text++;
		length = (unsigned int)strcspn(text, ",");
		if (length >= CONTROL_NAME_LENGTH)
			length = CONTROL_NAME_LENGTH - 1;
		memcpy(binding, text, length);
		binding[length] = 0;
		while (length && binding[length - 1] == ' ')
			binding[--length] = 0;
		text += strcspn(text, ",");
	}
}

static void control_bindings_text(short control, char *text, unsigned int size)
{
	char const *first = controls_screen.bindings[control][0];
	char const *second = controls_screen.bindings[control][1];

	snprintf(text, size, "%s%s%s", *first ? first : second, *first && *second ? ", " : "", *first ? second : "");
}

/* "controls screen init" (and "controls screen defaults": the defaults) */
static boolean controls_load(boolean defaults)
{
	short control;

	for (control = 0; control < NUMBEROF(controls); control++)
	{
		char text[128];

		if (defaults)
			config_default(controls[control].setting, text, sizeof(text));
		else
			snprintf(text, sizeof(text), "%s", config_string(controls[control].setting));
		control_bindings_read(control, text);
	}
	controls_screen.capturing_control = NONE;
	return TRUE;
}

/* "controls screen change set" (OK) */
static boolean controls_save(void)
{
	short control;
	boolean result = TRUE;

	for (control = 0; control < NUMBEROF(controls); control++)
	{
		char text[128];

		control_bindings_text(control, text, sizeof(text));
		if (strcmp(text, config_string(controls[control].setting)))
		{
			if (config_write(controls[control].setting, text))
				platform_log("menus: %s = %s", controls[control].setting, text);
			else
				result = FALSE;
		}
	}
	return result;
}

static struct widget_instance *control_row(struct widget_instance *widget)
{
	for (; widget; widget = widget->parent)
	{
		if (!strncmp(widget->name, "op_command_", 11))
			return widget;
	}
	return NULL;
}

/* the control on the row (op_command_<n>) in the group shown, else NONE */
static short control_of_row(struct widget_instance *row, short group)
{
	short wanted = row ? (short)(atoi(row->name + 11) - 1) : NONE;
	short control;

	for (control = 0; control < NUMBEROF(controls) && wanted >= 0; control++)
	{
		if (controls[control].group == group && wanted-- == 0)
			return control;
	}
	return NONE;
}

static short controls_group(struct widget_instance *screen)
{
	struct widget_instance *spinner = named(screen, "group_spinner", 0);

	return spinner ? spinner->parameters.list.selected_index : 0;
}

/* "controls begin binding" (A on a row): the next key or button pressed */
static boolean control_capture_begin(struct widget_instance *widget)
{
	short control = control_of_row(control_row(widget), controls_group(screen_of(widget)));

	if (control == NONE)
		return FALSE;
	controls_screen.capturing_control = control;
	controls_screen.capturing_slot = controls_screen.slot;
	platform_binding_capture_begin();
	return TRUE;
}

/* the key or button taken (then bound to no other control), or cleared */
static void control_capture_poll(void)
{
	short control = controls_screen.capturing_control;
	short slot = controls_screen.capturing_slot;
	int input = -1;
	int result;

	if (control == NONE || !(result = platform_binding_capture_poll(&input)))
		return;
	controls_screen.capturing_control = NONE;
	if (result == 1)
	{
		char name[CONTROL_NAME_LENGTH];
		short other, other_slot;

		halo_input_name(input, name, sizeof(name));
		if (!*name)
			return;
		for (other = 0; other < NUMBEROF(controls); other++)
		{
			for (other_slot = 0; other_slot < CONTROL_BINDINGS; other_slot++)
			{
				if (!_stricmp(controls_screen.bindings[other][other_slot], name))
					controls_screen.bindings[other][other_slot][0] = 0;
			}
		}
		snprintf(controls_screen.bindings[control][slot], CONTROL_NAME_LENGTH, "%s", name);
	}
	else if (result == 2)
	{
		controls_screen.bindings[control][slot][0] = 0;
	}
}

/* "controls update menu": the group's rows, their bindings (the one left
and right choose marked on the row chosen), and the help */
static void controls_update(struct widget_instance *list)
{
	short group = controls_group(list);
	struct widget_instance *row;
	struct widget_instance *focused_row = control_row(list->focused_child);

	control_capture_poll();
	for (row = list->child; row; row = row->next)
	{
		short control = control_of_row(row, group);
		short slot;

		if (strncmp(row->name, "op_command_", 11))
			continue;
		row->visible = control != NONE;
		if (control == NONE)
		{
			if (row == focused_row)
				focus_row(list, 0);
			continue;
		}
		text_set(named(row, "command_label", 0), controls[control].label);
		for (slot = 0; slot < CONTROL_BINDINGS; slot++)
		{
			char const *binding = controls_screen.bindings[control][slot];
			wchar_t text[ROW_TEXT_LENGTH];
			char shown[64];
			short index;

			if (controls_screen.capturing_control == control && controls_screen.capturing_slot == slot)
				snprintf(shown, sizeof(shown), "%s", "PRESS A KEY");
			else if (row == focused_row && controls_screen.slot == slot)
				snprintf(shown, sizeof(shown), "> %s <", *binding ? binding : "-");
			else
				snprintf(shown, sizeof(shown), "%s", *binding ? binding : "-");
			for (index = 0; shown[index] && index < ROW_TEXT_LENGTH - 1; index++)
				text[index] = (wchar_t)(unsigned char)shown[index];
			text[index] = 0;
			text_set(named(row, "command_binding", slot), text);
		}
	}
	if (list->parameters.list.extended_description)
	{
		list->parameters.list.extended_description->parameters.text_box.string_list_index =
			controls_screen.capturing_control != NONE ? 2 : focused_row ? 1 : 0;
	}
}

/* ---------- Multiplayer (port/assets/menus' PLAN: Create Game > Internet
or LAN; Join Game > Server Browser (blank, for now), LAN, Direct Link): the
Xbox's networking, run by the engine's port entry points
(ui_widget_event_handler_functions.c) on our lists */

#define MAXIMUM_ADVERTISED_GAMES 9
#define MAP_ROWS 11
#define GAMETYPE_ROWS 10
#define MAXIMUM_GAMETYPES 100
#define BROWSER_ROWS 15
#define LOBBY_ROWS 11
#define TEXT_FIELD_LENGTH 128
#define PLAYLIST_READ_ONLY_BIT 0x40000000UL
/* (a key stroke's modifier, as input_xbox.c has them: shift, control) */
#define KEY_MODIFIER_CONTROL_BIT 1
#define LOBBY_NAME "pc\\main_menu\\multiplayer_type_select\\lobby\\lobby_screen"
#define PREVIEW_NAME "pc\\main_menu\\multiplayer_type_select\\lobby\\preview_screen"
/* the lobby's panel's lines (the gametype, the players, the countdown, the
invite) */
#define LOBBY_TEXT_LENGTH (ROW_TEXT_LENGTH * 4)

enum
{
	_multiplayer_mode_server_browser,
	_multiplayer_mode_lan,
	_multiplayer_mode_direct_link,
	_multiplayer_mode_host_internet,
	_multiplayer_mode_host_lan,
};

enum
{
	_client_state_searching,
	_client_state_joining,
	_client_state_pregame,
	_client_state_ingame,
	_client_state_postgame,
};

/* a game the client found, as network_client_manager.c has it */
struct advertised_game
{
	byte key_id[8];
	byte key[16];
	/* XNADDR: size, flags, abEnet (the port's machine identifier), ina */
	byte xnaddr[12];
	byte nonce[8];
	unsigned long update_time;
	wchar_t game_name[16];
	long map_version;
	char map_name[0x80];
	short engine_type;
	word machine_count;
	word player_count;
	short maximum_player_count;
	short unknown100;
	short platform;
	boolean open;
	boolean valid;
	boolean has_teams;
	boolean oddball_variant;
};

typedef char verify_advertised_game_platform_offset[offsetof(struct advertised_game, platform) == 0xDE ? 1 : -1];
typedef char verify_advertised_game_open_offset[offsetof(struct advertised_game, open) == 0xE0 ? 1 : -1];

/* the engine's (port) */
short ui_widget_port_multiplayer_maps(char const *const **names, short *last_used);
boolean ui_widget_port_multiplayer_map_choose(short level_index);
short ui_widget_port_gametypes(long *indices, short maximum, short *last_used);
boolean ui_widget_port_gametype_choose(long profile_index);
boolean ui_widget_port_host(struct widget_instance *widget, struct event_record *event, boolean *widget_deleted);
boolean ui_widget_port_browse(struct widget_instance *widget, struct event_record *event, boolean *widget_deleted);
boolean ui_widget_port_open(struct widget_instance *widget, char const *name, boolean *widget_deleted);
boolean network_game_client_advertised_game_in_progress(void *client, struct advertised_game *game);
boolean ui_widget_port_join(struct widget_instance *widget, void *advertised_game, char const *lobby_name,
	boolean *widget_deleted);
boolean ui_widget_port_multiplayer_player(short controller_index, long profile_index);
void network_game_server_port_set_settings(wchar_t const *name, long maximum_players);
void *global_network_game_client_get(void);
void *global_network_game_server_get(void);
struct advertised_game *network_game_client_get_available_games(void *client);
boolean network_game_client_advertised_game_is_valid(struct advertised_game *game);
short network_game_client_get_state(void *client, short *state_data);
struct network_game *network_game_client_get_game(void *client);
short network_game_client_get_local_machine_index(void);
short network_game_client_get_seconds_to_game_start(void *client);
boolean network_player_is_valid(struct network_player *player);
boolean playlist_profile_get(long index, struct game_variant *variant);
boolean playlist_profile_get_display_name(long index, wchar_t *name);
boolean input_get_key(struct key_stroke *key);
/* the platform layer's */
int p2p_join_invite(char const *text);
int p2p_invite_link(char *link, int size);
void p2p_set_hosting_allowed(int allowed);
int p2p_peer_address(unsigned char const *identifier, unsigned long *address);
int platform_clipboard_get(char *text, int size);
void platform_clipboard_set(char const *text);
void platform_text_field(int typing);
int config_boolean(char const *name);

static wchar_t const *const engine_names[] = { L"", L"CTF", L"SLAYER", L"ODDBALL", L"KING OF THE HILL", L"RACE" };
static short const maximum_players[] = { 2, 4, 8, 12, 16, 24, 32, 48, 64, 96, 128 };

static struct
{
	short mode;
	/* Create Game's map and gametype lists */
	short map_first, map_chosen;
	long gametypes[MAXIMUM_GAMETYPES];
	short gametype_count;
	/* (those of the bank the list's spinner shows: STANDARD, CUSTOM) */
	short bank[MAXIMUM_GAMETYPES], bank_count, bank_shown;
	short gametype_first, gametype_chosen;
	/* the server settings */
	wchar_t game_name[16];
	short maximum_players_index;
	/* the browser's games */
	struct advertised_game *games[MAXIMUM_ADVERTISED_GAMES];
	short game_count, game_chosen;
	/* the lobby's */
	short lobby_first;
	/* the game under way whose lobby is shown before joining it: its key
	(the client's slot it is in may come to hold another game) */
	byte preview_key_id[8];
	byte preview_xnaddr[12];
} multiplayer = { 0, 0, 0, { 0 }, 0, { 0 }, 0, 0, 0, 0, { 0 }, NUMBEROF(maximum_players) - 1 };

static char const *const *multiplayer_map_names;
static short multiplayer_map_count;

/* ---- a text field (Direct Link's link, the game's name): the keyboard
types into it (Ctrl+V pastes), its row's A (enter) is done, B (escape)
cancels */
static struct
{
	struct widget_instance *row;
	char text[TEXT_FIELD_LENGTH];
	char before[TEXT_FIELD_LENGTH];
	short maximum;
	void (*done)(char const *text);
} text_field;

/* when the field was last shown (a field not shown for a while is let go
of: its screen has gone) */
static unsigned long text_field_shown_time;

static boolean text_field_editing(struct widget_instance *row)
{
	return text_field.row && (!row || text_field.row == row);
}

static void text_field_begin(struct widget_instance *row, char const *text, short maximum,
	void (*done)(char const *text))
{
	struct key_stroke key;

	text_field.row = row;
	snprintf(text_field.text, sizeof(text_field.text), "%s", text);
	snprintf(text_field.before, sizeof(text_field.before), "%s", text);
	text_field.maximum = (short)MIN(maximum, TEXT_FIELD_LENGTH - 1);
	text_field.done = done;
	text_field_shown_time = system_milliseconds();
	while (input_get_key(&key))
		;
	platform_text_field(TRUE);
}

static void text_field_end(boolean keep)
{
	void (*done)(char const *text) = text_field.done;

	platform_text_field(FALSE);
	text_field.row = NULL;
	text_field.done = NULL;
	if (!keep)
		snprintf(text_field.text, sizeof(text_field.text), "%s", text_field.before);
	else if (done)
		done(text_field.text);
}

static void text_field_insert(char const *text)
{
	size_t length = strlen(text_field.text);

	for (; *text && length < (size_t)text_field.maximum; text++)
	{
		if (*text >= ' ' && *text <= '~')
			text_field.text[length++] = *text;
	}
	text_field.text[length] = 0;
}

/* the keys typed since the last frame, and the field shown (a caret
blinking while it is edited) */

static void text_field_show(struct widget_instance *value, char const *text, boolean editing)
{
	wchar_t shown[TEXT_FIELD_LENGTH + 2];
	short index;

	if (editing)
	{
		text_field_shown_time = system_milliseconds();
		struct key_stroke key;

		while (input_get_key(&key))
		{
			size_t length = strlen(text_field.text);

			if (key.key_code == _key_backspace)
			{
				if (length)
					text_field.text[length - 1] = 0;
			}
			else if (TEST_FLAG(key.modifier_flags, KEY_MODIFIER_CONTROL_BIT) && key.key_code == _key_v)
			{
				char clipboard[TEXT_FIELD_LENGTH];

				if (platform_clipboard_get(clipboard, sizeof(clipboard)))
					text_field_insert(clipboard);
			}
			else if ((unsigned char)key.ascii_code >= ' ' && (unsigned char)key.ascii_code <= '~')
			{
				char character[2] = { key.ascii_code, 0 };

				text_field_insert(character);
			}
		}
		text = text_field.text;
	}
	for (index = 0; text[index] && index < TEXT_FIELD_LENGTH; index++)
		shown[index] = (wchar_t)(unsigned char)text[index];
	if (editing && (system_milliseconds() / 500) % 2)
		shown[index++] = L'_';
	shown[index] = 0;
	text_set(value, shown);
}

/* ---- the menu */

/* "mp type set mode": the item's (by its name) */
static void multiplayer_mode_set(struct widget_instance *widget)
{
	char const *name = widget->name;

	/* (an instance's name is cut to 31 characters:
	"multiplayer_type_create_interne") */
	multiplayer.mode =
		strstr(name, "create_inter") ? _multiplayer_mode_host_internet :
		strstr(name, "create_lan") ? _multiplayer_mode_host_lan :
		strstr(name, "join_lan") ? _multiplayer_mode_lan :
		strstr(name, "join_direct") ? _multiplayer_mode_direct_link : _multiplayer_mode_server_browser;
}

/* player 1's profile for the controller's player in a network game */
static boolean multiplayer_player(short controller)
{
	struct player_profile profile;

	if (!campaign_profile(controller, &profile))
		return campaign_fail();
	return ui_widget_port_multiplayer_player(controller, player_ui_get_active_player_profile_index(0));
}

/* Create Game's ("join controller to mp game", first): player 1 in, and
the server made: an internet one (an invite, Discord) or a LAN one */
static boolean multiplayer_host(struct widget_instance *widget, struct event_record *event, short controller,
	boolean *widget_deleted)
{
	multiplayer_mode_set(widget);
	if (!multiplayer_player(controller))
		return FALSE;
	p2p_set_hosting_allowed(multiplayer.mode == _multiplayer_mode_host_internet);
	return ui_widget_port_host(widget, event, widget_deleted);
}

/* ---- the map list */

static void map_name_text(char const *name, wchar_t *text)
{
	char caption[HALO_CUSTOM_MAP_NAME_SIZE];
	short index;

	native_map_display_name(name, caption, sizeof(caption));
	for (index = 0; caption[index]; index++) text[index] = (unsigned char)caption[index];
	text[index] = 0;
}

/* These preview boxes also show authored stock strings. Resize even when
   one of those shorter strings already allocated their text buffer. */
static void map_caption_set(struct widget_instance *widget, wchar_t const *caption)
{
	wchar_t *text;
	if (!widget) return;
	text = ui_widget_realloc(widget->parameters.text_box.text,
		ROW_TEXT_LENGTH * sizeof(wchar_t), __FILE__, __LINE__);
	if (!text) return;
	widget->parameters.text_box.text = text;
	ustrncpy(text, caption, ROW_TEXT_LENGTH - 1);
	text[ROW_TEXT_LENGTH - 1] = 0;
	widget->parameters.text_box.string_list_index = HALO_CUSTOM_MAP_TEXT;
}

static void map_row_text(short row, wchar_t *text)
{
	short map = (short)(multiplayer.map_first + row);
	text[0] = 0;
	if (map < 0 || map >= multiplayer_map_count) return;
	if (map < HALO_STOCK_MULTIPLAYER_MAP_COUNT)
		string_get("pc\\main_menu\\mp_map_list", map, text);
	else
		map_name_text(multiplayer_map_names[map], text);
}

/* "mp level list initialize" */
static boolean map_list_initialize(struct widget_instance *list)
{
	multiplayer_map_count = ui_widget_port_multiplayer_maps(&multiplayer_map_names, &multiplayer.map_chosen);
	multiplayer.map_first = (short)PIN(multiplayer.map_chosen - MAP_ROWS / 2,
		0, MAX(0, multiplayer_map_count - MAP_ROWS));
	focus_row(list, (short)(multiplayer.map_chosen - multiplayer.map_first));
	return TRUE;
}

/* "mp map list update" */
static void map_list_update(struct widget_instance *list)
{
	struct widget_instance *description = list->parameters.list.extended_description;
	struct widget_instance *widget;
	short map = list_scroll(list, &multiplayer.map_first, multiplayer_map_count, MAP_ROWS);
	wchar_t caption[ROW_TEXT_LENGTH];

	if (map >= 0 && map < multiplayer_map_count)
		multiplayer.map_chosen = map;
	rows_update(list, (short)MIN(MAP_ROWS, MAX(0, multiplayer_map_count - multiplayer.map_first)), map_row_text);
	if (multiplayer.map_chosen < 0 || multiplayer.map_chosen >= multiplayer_map_count) return;
	if (multiplayer.map_chosen >= HALO_STOCK_MULTIPLAYER_MAP_COUNT)
	{
		map_name_text(multiplayer_map_names[multiplayer.map_chosen], caption);
		map_caption_set(named(description, "mp_map_right_name", 0), caption);
		map_caption_set(named(description, "mp_map_right_data", 0), L"Community map");
		if ((widget = named(description, "mp_map_right_pic", 0)) != NULL)
			widget->animation.current_frame_index = 19;
		profile_name_show(description);
		return;
	}
	if ((widget = named(description, "mp_map_right_name", 0)) != NULL)
		widget->parameters.text_box.string_list_index = multiplayer.map_chosen;
	if ((widget = named(description, "mp_map_right_pic", 0)) != NULL)
		widget->animation.current_frame_index = multiplayer.map_chosen;
	if ((widget = named(description, "mp_map_right_data", 0)) != NULL)
		widget->parameters.text_box.string_list_index = multiplayer.map_chosen;
	profile_name_show(description);
}

/* (the gametype editor's: Server Setup's copy of the game's gametype) */
static void gametype_setup_begin(void);
static void gametype_setup_end(void);
static boolean gametype_setup_apply(void);
static void gametype_setup_type(wchar_t *text);

/* ---- the gametype list: its spinner's bank (STANDARD: the built-in ones,
CUSTOM: those saved) in its first row, then the bank's gametypes */

static void gametype_bank_read(short bank)
{
	short index;

	multiplayer.bank_shown = bank;
	multiplayer.bank_count = 0;
	for (index = 0; index < multiplayer.gametype_count; index++)
	{
		boolean standard = ((unsigned long)multiplayer.gametypes[index] & PLAYLIST_READ_ONLY_BIT) != 0;

		if (standard == (bank == 0))
			multiplayer.bank[multiplayer.bank_count++] = index;
	}
	multiplayer.gametype_first = 0;
	multiplayer.gametype_chosen = 0;
}

/* "mp profiles list initialize": on the gametype used last */
static boolean gametype_list_initialize(struct widget_instance *list)
{
	struct widget_instance *spinner = named(list, "list_item_0_chooser_spinner", 0);
	short last, index;

	multiplayer.gametype_count = ui_widget_port_gametypes(multiplayer.gametypes, MAXIMUM_GAMETYPES, &last);
	gametype_bank_read(multiplayer.gametype_count && !((unsigned long)multiplayer.gametypes[last] &
		PLAYLIST_READ_ONLY_BIT) ? 1 : 0);
	for (index = 0; index < multiplayer.bank_count; index++)
	{
		if (multiplayer.bank[index] == last)
			multiplayer.gametype_chosen = index;
	}
	multiplayer.gametype_first = (short)PIN(multiplayer.gametype_chosen - GAMETYPE_ROWS / 2, 0,
		MAX(0, multiplayer.bank_count - GAMETYPE_ROWS));
	if (spinner)
		spinner->parameters.list.selected_index = multiplayer.bank_shown;
	focus_row(list, (short)(1 + multiplayer.gametype_chosen - multiplayer.gametype_first));
	return TRUE;
}

/* a gametype's name, in a row's text (the name's own buffer is longer:
playlist_profile_get_display_name fills MAX_GAMENAME characters) */
static void gametype_display_name(long profile_index, wchar_t *text)
{
	wchar_t name[MAX_GAMENAME];

	text[0] = 0;
	if (playlist_profile_get_display_name(profile_index, name))
	{
		ustrncpy(text, name, ROW_TEXT_LENGTH - 1);
		text[ROW_TEXT_LENGTH - 1] = 0;
	}
}

static void gametype_name(short item, wchar_t *text)
{
	text[0] = 0;
	if (item >= 0 && item < multiplayer.bank_count)
		gametype_display_name(multiplayer.gametypes[multiplayer.bank[item]], text);
}

/* "gt select list update": the bank shown, its rows (from the second), and
the gametype chosen: its name, picture and rules */
static void gametype_list_update(struct widget_instance *list)
{
	struct widget_instance *description = list->parameters.list.extended_description;
	struct widget_instance *spinner = named(list, "list_item_0_chooser_spinner", 0);
	struct widget_instance *row;
	struct widget_instance *widget;
	short row_index = 0, focused = NONE, count;

	if (spinner && spinner->parameters.list.selected_index != multiplayer.bank_shown)
		gametype_bank_read(spinner->parameters.list.selected_index);
	count = (short)MIN(multiplayer.bank_count, GAMETYPE_ROWS);
	/* (the rows after the spinner's: scrolled as list_scroll does) */
	for (row = list->child; row; row = row->next, row_index++)
	{
		if (row == list->focused_child && row_index >= 1 && row_index <= GAMETYPE_ROWS)
			focused = (short)(row_index - 1);
	}
	if (focused == count - 1 && multiplayer.gametype_first + count < multiplayer.bank_count)
	{
		multiplayer.gametype_first++;
		focus_row(list, (short)focused);
		focused--;
	}
	else if (focused == 0 && multiplayer.gametype_first > 0)
	{
		multiplayer.gametype_first--;
		focus_row(list, 2);
		focused++;
	}
	if (focused != NONE)
		multiplayer.gametype_chosen = (short)(multiplayer.gametype_first + focused);
	for (row = list->child, row_index = 0; row; row = row->next, row_index++)
	{
		wchar_t text[ROW_TEXT_LENGTH];

		if (row_index < 1 || row_index > GAMETYPE_ROWS)
			continue;
		row->visible = row_index - 1 < count;
		gametype_name((short)(multiplayer.gametype_first + row_index - 1), text);
		text_set(named(row, "list_item_text", 0), text);
		if (named(row, "list_item_arrows", 0))
			named(row, "list_item_arrows", 0)->visible = row == list->focused_child;
		if (named(row, "scroll_up_button", 0))
			named(row, "scroll_up_button", 0)->visible = FALSE;
		if (named(row, "scroll_down_button", 0))
			named(row, "scroll_down_button", 0)->visible = FALSE;
	}
	visible_set(named(description, "locked_gametype_icon", 0), FALSE);
	visible_set(named(description, "gametype_right_item", 0), multiplayer.bank_count > 0);
	if (multiplayer.gametype_chosen < multiplayer.bank_count)
	{
		struct game_variant variant;
		wchar_t text[ROW_TEXT_LENGTH * 2];

		gametype_name(multiplayer.gametype_chosen, text);
		text_set(named(description, "gametype_right_name", 0), text);
		if (playlist_profile_get(multiplayer.gametypes[multiplayer.bank[multiplayer.gametype_chosen]], &variant))
		{
			long engine = PIN(variant.game_engine_index, 0, 5);

			if ((widget = named(description, "gametype_right_pic", 0)) != NULL)
				widget->animation.current_frame_index = (short)engine;
			usnprintf(text, NUMBEROF(text) - 1, L"%s\r\n%s\r\nScore to win: %ld", engine_names[engine],
				variant.universal_variant.teams ? L"Teams" : L"Free for all", variant.universal_variant.score_to_win);
			text[NUMBEROF(text) - 1] = 0;
			text_set(named(description, "gametype_right_data", 0), text);
		}
	}
	profile_name_show(description);
}

/* "mp profile set for game" */
static boolean gametype_choose(void)
{
	gametype_setup_end();
	if (multiplayer.gametype_chosen >= multiplayer.bank_count)
		return campaign_fail();
	return ui_widget_port_gametype_choose(multiplayer.gametypes[multiplayer.bank[multiplayer.gametype_chosen]]);
}

/* ---- the server settings */

static void game_name_done(char const *text)
{
	short index;

	for (index = 0; text[index] && index < NUMBEROF(multiplayer.game_name) - 1; index++)
		multiplayer.game_name[index] = (wchar_t)(unsigned char)text[index];
	multiplayer.game_name[index] = 0;
}

/* "server settings init": the game's name (player 1's, else the one given
last), the most players, the gametype's copy (once: the screen is made
again on coming back from an option's screen) */
static boolean server_settings_initialize(struct widget_instance *list)
{
	struct widget_instance *spinner = named(list, "max_players_spinner", 0);

	gametype_setup_begin();
	if (!multiplayer.game_name[0] && player_ui_get_active_player_profile_index(0) != NONE)
	{
		struct player_profile profile;

		player_ui_get_active_player_profile(0, &profile);
		ustrncpy(multiplayer.game_name, profile.player_name, NUMBEROF(multiplayer.game_name) - 1);
	}
	if (spinner)
		spinner->parameters.list.selected_index = multiplayer.maximum_players_index;
	return TRUE;
}

static void wide_to_text(wchar_t const *wide, char *text, short size)
{
	short index;

	for (index = 0; wide[index] && index < size - 1; index++)
		text[index] = wide[index] < 0x80 ? (char)wide[index] : '?';
	text[index] = 0;
}

/* "server settings update" */
static void server_settings_update(struct widget_instance *list)
{
	struct widget_instance *spinner = named(list, "max_players_spinner", 0);
	struct widget_instance *row = named(list, "op_server_name", 0);
	char text[TEXT_FIELD_LENGTH];

	if (spinner)
		multiplayer.maximum_players_index = (short)PIN(spinner->parameters.list.selected_index, 0,
			NUMBEROF(maximum_players) - 1);
	wide_to_text(multiplayer.game_name, text, sizeof(text));
	text_field_show(named(list, "server_name_value", 0), text, text_field_editing(row));
	if (multiplayer.mode != _multiplayer_mode_host_internet)
		snprintf(text, sizeof(text), "NONE: A LAN GAME");
	else if (!config_boolean("network.online"))
		snprintf(text, sizeof(text), "INTERNET PLAY IS OFF (SETTINGS)");
	else if (!p2p_invite_link(text, sizeof(text)))
		snprintf(text, sizeof(text), "MADE WHEN THE GAME STARTS");
	text_field_show(named(list, "invite_value", 0), text, FALSE);
	{
		wchar_t type[ROW_TEXT_LENGTH];

		gametype_setup_type(type);
		text_set(named(list, "game_type_value", 0), type);
	}
	settings_help(list);
}

/* "ss edit server name" (its row's A: begins, or ends) */
static boolean server_name_edit(struct widget_instance *row)
{
	char text[TEXT_FIELD_LENGTH];

	if (text_field_editing(row))
	{
		text_field_end(TRUE);
		return TRUE;
	}
	wide_to_text(multiplayer.game_name, text, sizeof(text));
	text_field_begin(row, text, NUMBEROF(multiplayer.game_name) - 1, game_name_done);
	return TRUE;
}

/* "ss copy invite" */
static boolean invite_copy(void)
{
	char link[TEXT_FIELD_LENGTH];

	if (!p2p_invite_link(link, sizeof(link)))
		return campaign_fail();
	platform_clipboard_set(link);
	ui_play_audio_feedback_sound(SOUND_FORWARD);
	return TRUE;
}

/* "ss start game": the settings given the server, then the lobby */
static boolean server_start(void)
{
	if (text_field_editing(NULL))
		text_field_end(TRUE);
	network_game_server_port_set_settings(multiplayer.game_name,
		maximum_players[PIN(multiplayer.maximum_players_index, 0, NUMBEROF(maximum_players) - 1)]);
	/* (the gametype as Server Setup's options left it) */
	if (!gametype_setup_apply())
		return campaign_fail();
	return global_network_game_server_get() != NULL;
}

/* ---- the browser (Join Game > Server Browser, LAN, Direct Link): the
games found on the LAN, or (Direct Link) from internet play's peers
(reached by an invite link, the clipboard, or Discord) */

#define JOIN_GAME_LABELS "pc\\main_menu\\multiplayer_type_select\\join_game\\join_game_ticker_labels"
enum { _join_game_label_servers = 6, _join_game_label_players, _join_game_label_page };

static boolean game_from_peer(struct advertised_game const *game)
{
	unsigned long address;

	return p2p_peer_address(game->xnaddr + 2, &address) != 0;
}

static boolean advertised_in_progress(struct advertised_game *game)
{
	return network_game_client_advertised_game_in_progress(global_network_game_client_get(), game);
}

static void browser_games_read(void)
{
	void *client = global_network_game_client_get();
	struct advertised_game *games = client ? network_game_client_get_available_games(client) : NULL;
	short pass, index;

	multiplayer.game_count = 0;
	if (!games || multiplayer.mode == _multiplayer_mode_server_browser)
		return;
	/* (the open games, then those under way) */
	for (pass = 0; pass < 2; pass++)
	{
		for (index = 0; index < MAXIMUM_ADVERTISED_GAMES; index++)
		{
			struct advertised_game *game = &games[index];

			if (network_game_client_advertised_game_is_valid(game) && !advertised_in_progress(game) == (pass == 0) &&
				game_from_peer(game) == (multiplayer.mode == _multiplayer_mode_direct_link))
			{
				multiplayer.games[multiplayer.game_count++] = game;
			}
		}
	}
	if (multiplayer.game_chosen >= multiplayer.game_count)
		multiplayer.game_chosen = (short)MAX(0, multiplayer.game_count - 1);
}

static short browser_row_index(struct widget_instance *row)
{
	return row && !strncmp(row->name, "server_item_", 12) ? (short)(atoi(row->name + 12) - 1) : NONE;
}

/* a bar of buttons' focus, off one hidden: on its first one shown */
static void focus_off_hidden(struct widget_instance *bar)
{
	struct widget_instance *button;
	short button_index = 0;

	for (button = bar && bar->focused_child && !bar->focused_child->visible ? bar->child : NULL; button;
		button = button->next, button_index++)
	{
		if (button->visible)
		{
			bar->focused_child = button;
			bar->parameters.list.selected_index = button_index;
			break;
		}
	}
}

/* whether the browser's list's child can take focus (once visible) */
static boolean browser_item_usable(struct widget_instance *child)
{
	return browser_row_index(child) != NONE || !strcmp(child->name, "join_game_button_bar");
}

/* the browser's focus, off what cannot have it (at the start, or a row whose
game went): the first game, else the buttons (off those hidden) */
static void browser_focus(struct widget_instance *list)
{
	char const *const choices[] = { "server_item_1", "join_game_button_bar" };
	struct widget_instance *focused = list->focused_child;
	short index;

	focus_off_hidden(named(list, "join_game_button_bar", 0));
	if (focused && focused->visible && !focused->disabled)
		return;
	for (index = 0; index < NUMBEROF(choices); index++)
	{
		struct widget_instance *child;
		short child_index = 0;

		for (child = list->child; child; child = child->next, child_index++)
		{
			if (!strcmp(child->name, choices[index]) && child->visible)
			{
				list->focused_child = child;
				list->parameters.list.selected_index = child_index;
				return;
			}
		}
	}
}

/* "gamespy screen init": the mode's title and parts; the games' client */
static boolean browser_initialize(struct widget_instance *screen, struct event_record *event,
	boolean *widget_deleted)
{
	char const *title = multiplayer.mode == _multiplayer_mode_lan ? "header_lan" :
		multiplayer.mode == _multiplayer_mode_direct_link ? "header_direct_link" : "header_server_browser";
	char const *const titles[] = { "header_internet", "header_lan", "header_direct_link", "header_server_browser" };
	char const *const unused[] = { "op_browser_mode", "join_game_button_update", "join_game_button_filters" };
	short index;

	server_browser_initialize(screen);
	for (index = 0; index < NUMBEROF(titles); index++)
		visible_set(named(screen, titles[index], 0), !strcmp(titles[index], title));
	for (index = 0; index < NUMBEROF(unused); index++)
		visible_set(named(screen, unused[index], 0), FALSE);
	visible_set(named(screen, "button_clipboard", 0), multiplayer.mode == _multiplayer_mode_direct_link);
	{
		struct widget_instance *list = named(screen, "join_game_items_list", 0);
		struct widget_instance *child;

		/* (only the games' rows and the buttons take focus: not the column titles, which sort nothing, nor the ticker) */
		for (child = list ? list->child : NULL; child; child = child->next)
			child->disabled = !browser_item_usable(child);
		if (list)
			browser_focus(list);
	}
	visible_set(named(screen, "scroll_up_button", 0), FALSE);
	visible_set(named(screen, "scroll_down_button", 0), FALSE);
	/* (the columns' sort arrows, both of each over its title: the list is
	not sorted) */
	for (index = 0; named(screen, "header_sort_arrows", index); index++)
		visible_set(named(screen, "header_sort_arrows", index), FALSE);
	multiplayer.game_chosen = 0;
	if (multiplayer.mode == _multiplayer_mode_server_browser)
		return TRUE;
	return ui_widget_port_browse(screen, event, widget_deleted);
}

static void game_map_name(struct advertised_game const *game, wchar_t *text)
{
	char const *const *names;
	short last, index;
	short count = ui_widget_port_multiplayer_maps(&names, &last);

	for (index = 0; index < count; index++)
	{
		if (!_stricmp(native_map_basename(names[index]), native_map_basename(game->map_name)))
		{
			if (index < HALO_STOCK_MULTIPLAYER_MAP_COUNT)
				string_get("pc\\main_menu\\mp_map_list", index, text);
			else
				map_name_text(names[index], text);
			return;
		}
	}
	for (index = 0; game->map_name[index] && index < ROW_TEXT_LENGTH - 1; index++)
		text[index] = (wchar_t)(unsigned char)game->map_name[index];
	text[index] = 0;
}

/* "gamespy screen update": the games' rows (name, map, gametype, players),
the one chosen's line, the counts over the titles (the PC version's: its
players, page, servers) */
static void browser_update(struct widget_instance *list)
{
	struct widget_instance *row;
	struct widget_instance *stats = list->parameters.list.extended_description;
	short focused = browser_row_index(list->focused_child);

	browser_games_read();
	if (focused != NONE && focused < multiplayer.game_count)
		multiplayer.game_chosen = focused;
	for (row = list->child; row; row = row->next)
	{
		short index = browser_row_index(row);
		struct advertised_game *game;
		wchar_t text[ROW_TEXT_LENGTH];

		if (index == NONE)
			continue;
		row->visible = index < multiplayer.game_count;
		if (index >= multiplayer.game_count)
			continue;
		game = multiplayer.games[index];
		ustrncpy(text, game->game_name, NUMBEROF(game->game_name) - 1);
		text[NUMBEROF(game->game_name) - 1] = 0;
		text_set(named(row, "server_item_server_name", 0), text);
		game_map_name(game, text);
		text_set(named(row, "server_item_map", 0), text);
		text_set(named(row, "server_item_type", 0), engine_names[PIN(game->engine_type, 0, 5)]);
		usnprintf(text, ROW_TEXT_LENGTH - 1, L"%d/%d", game->player_count, game->maximum_player_count);
		text_set(named(row, "server_item_players", 0), text);
		text_set(named(row, "server_item_ping", 0), advertised_in_progress(game) ? L"LIVE" : L"");
		visible_set(named(row, "server_item_locked", 0), FALSE);
		visible_set(named(row, "server_item_dedicated", 0), FALSE);
		visible_set(named(row, "server_item_classic", 0), FALSE);
	}
	{
		wchar_t text[ROW_TEXT_LENGTH * 2];

		if (multiplayer.mode == _multiplayer_mode_server_browser)
			text[0] = 0;
		else if (!multiplayer.game_count)
		{
			usnprintf(text, NUMBEROF(text) - 1, L"%s", multiplayer.mode == _multiplayer_mode_direct_link ?
				L"Copy an invite link and PASTE LINK, or accept a Discord invite" :
				L"Looking for games on your LAN...");
		}
		else
		{
			struct advertised_game *game = multiplayer.games[multiplayer.game_chosen];

			usnprintf(text, NUMBEROF(text) - 1, L"%d players of %d, on %d machines%s", game->player_count,
				game->maximum_player_count, game->machine_count, advertised_in_progress(game) ? L": under way" : L"");
		}
		text[NUMBEROF(text) - 1] = 0;
		text_set(named(list, "ticker_player_info", 0), text);
		text_set(named(list, "ticker_rules_info", 0), L"");
	}
	if (stats)
	{
		wchar_t label[ROW_TEXT_LENGTH], text[ROW_TEXT_LENGTH];
		long players = 0;
		short index;

		for (index = 0; index < multiplayer.game_count; index++)
			players += multiplayer.games[index]->player_count;
		string_get(JOIN_GAME_LABELS, _join_game_label_players, label);
		usnprintf(text, ROW_TEXT_LENGTH - 1, L"%s %ld", label, players);
		text_set(named(stats, "player_count_label", 0), text);
		/* (one page: the list holds every game found) */
		string_get(JOIN_GAME_LABELS, _join_game_label_page, label);
		usnprintf(text, ROW_TEXT_LENGTH - 1, L"%s 1/1", label);
		text_set(named(stats, "page_count_label", 0), text);
		string_get(JOIN_GAME_LABELS, _join_game_label_servers, label);
		usnprintf(text, ROW_TEXT_LENGTH - 1, L"%s %d", label, multiplayer.game_count);
		text_set(named(stats, "server_count_label", 0), text);
	}
	browser_focus(list);
}

/* "direct ip connect go" (Direct Link's PASTE LINK): the invite
link on the clipboard reached, its game then in the list */
static boolean direct_link_from_clipboard(void)
{
	char text[TEXT_FIELD_LENGTH];
	char *link = text;
	size_t length;

	if (!platform_clipboard_get(text, sizeof(text)))
		text[0] = 0;
	while (*link == ' ' || *link == '\t' || *link == '\r' || *link == '\n')
		link++;
	for (length = strlen(link); length && (link[length - 1] == ' ' || link[length - 1] == '\t' ||
		link[length - 1] == '\r' || link[length - 1] == '\n'); length--)
		link[length - 1] = 0;
	if (!*link || !p2p_join_invite(link))
	{
		platform_log("menus: the clipboard has no invite link");
		return campaign_fail();
	}
	return TRUE;
}

/* joining the game chosen: player 1 in, then the lobby */
static boolean browser_join(struct widget_instance *widget, short controller, boolean *widget_deleted)
{
	if (multiplayer.game_chosen >= multiplayer.game_count)
		return campaign_fail();
	if (!multiplayer_player(controller))
		return FALSE;
	return ui_widget_port_join(widget, multiplayer.games[multiplayer.game_chosen], LOBBY_NAME, widget_deleted);
}

/* "gamespy select item" (a row: joins its game) and "gamespy select button"
(Join) */
static boolean browser_select(struct widget_instance *widget, struct event_record *event, short controller,
	boolean *widget_deleted)
{
	short row = browser_row_index(widget);

	if (row != NONE)
		multiplayer.game_chosen = row;
	if (row != NONE || strstr(widget->name, "button_join"))
	{
		/* (a game under way: its lobby first, then JOIN GAME) */
		if (multiplayer.game_chosen < multiplayer.game_count &&
			advertised_in_progress(multiplayer.games[multiplayer.game_chosen]))
		{
			csmemcpy(multiplayer.preview_key_id, multiplayer.games[multiplayer.game_chosen]->key_id,
				sizeof(multiplayer.preview_key_id));
			csmemcpy(multiplayer.preview_xnaddr, multiplayer.games[multiplayer.game_chosen]->xnaddr,
				sizeof(multiplayer.preview_xnaddr));
			return ui_widget_port_open(widget, PREVIEW_NAME, widget_deleted);
		}
		return browser_join(widget, controller, widget_deleted);
	}
	return TRUE;
}

/* ---- the lobby: the game's players (up to the port's 128), its map and
gametype, the countdown */

static struct network_player *lobby_players[HALO_PORT_MAXIMUM_NETWORK_PLAYERS];
static short lobby_player_count;

static void lobby_row_text(short row, wchar_t *text)
{
	struct network_player *player = lobby_players[multiplayer.lobby_first + row];
	struct network_game *game = network_game_client_get_game(global_network_game_client_get());
	wchar_t name[NUMBEROF(player->name) + 1];

	ustrncpy(name, player->name, NUMBEROF(player->name));
	name[NUMBEROF(player->name)] = 0;
	if (game && game->variant.universal_variant.teams)
		usnprintf(text, ROW_TEXT_LENGTH - 1, L"%s  (%s)", name, player->team_index ? L"BLUE" : L"RED");
	else
		usnprintf(text, ROW_TEXT_LENGTH - 1, L"%s", name);
	text[ROW_TEXT_LENGTH - 1] = 0;
}

/* the lobby's panel's map: its picture and name */
static void lobby_map_show(struct widget_instance *description, char const *map_name)
{
	char const *const *names;
	short last, count = ui_widget_port_multiplayer_maps(&names, &last), map = 19, index;
	struct widget_instance *widget;

	for (index = 0; index < count; index++)
	{
		if (index < HALO_STOCK_MULTIPLAYER_MAP_COUNT &&
			!_stricmp(native_map_basename(names[index]), native_map_basename(map_name)))
			map = index;
	}
	if ((widget = named(description, "lobby_map_pic", 0)) != NULL)
		widget->animation.current_frame_index = map;
	if ((widget = named(description, "lobby_map_name", 0)) != NULL)
	{
		if (map == 19 && native_map_is_custom(map_name))
		{
			wchar_t caption[ROW_TEXT_LENGTH];
			map_name_text(map_name, caption);
			map_caption_set(widget, caption);
		}
		else
			widget->parameters.text_box.string_list_index = map;
	}
}

/* "port lobby update" */
static void lobby_update(struct widget_instance *list)
{
	struct widget_instance *description = list->parameters.list.extended_description;
	void *client = global_network_game_client_get();
	struct network_game *game = client ? network_game_client_get_game(client) : NULL;
	short state_data, state = client ? network_game_client_get_state(client, &state_data) : NONE;
	wchar_t text[LOBBY_TEXT_LENGTH];
	short index;

	lobby_player_count = 0;
	for (index = 0; game && state >= _client_state_pregame && index < HALO_PORT_MAXIMUM_NETWORK_PLAYERS; index++)
	{
		if (network_player_is_valid(&game->players[index]))
			lobby_players[lobby_player_count++] = &game->players[index];
	}
	list_scroll(list, &multiplayer.lobby_first, lobby_player_count, LOBBY_ROWS);
	if (multiplayer.lobby_first > MAX(0, lobby_player_count - LOBBY_ROWS))
		multiplayer.lobby_first = (short)MAX(0, lobby_player_count - LOBBY_ROWS);
	rows_update(list, (short)MIN(lobby_player_count, LOBBY_ROWS), lobby_row_text);
	visible_set(named(list, "lobby_button_team", 0), game && game->variant.universal_variant.teams);
	/* (the buttons' focus, off Switch Team when it is hidden) */
	focus_off_hidden(named(list, "lobby_button_bar", 0));
	visible_set(named(description, "lobby_right_item", 0), game != NULL && state >= _client_state_pregame);
	if (!game || state < _client_state_pregame)
	{
		profile_name_show(description);
		return;
	}
	lobby_map_show(description, game->map.name);
	{
		short seconds = network_game_client_get_seconds_to_game_start(client);
		char link[TEXT_FIELD_LENGTH];
		wchar_t gametype[NUMBEROF(game->variant.human_readable_game_description) + 1];

		ustrncpy(gametype, game->variant.human_readable_game_description, NUMBEROF(gametype) - 1);
		gametype[NUMBEROF(gametype) - 1] = 0;
		usnprintf(text, NUMBEROF(text) - 1, L"%s\r\n%s\r\n%d of %d players\r\n\r\n%s", gametype,
			engine_names[PIN(game->variant.game_engine_index, 0, 5)], lobby_player_count, game->maximum_players,
			seconds > 0 ? L"Starting in:" : game->machine_count < 2 ? L"Waiting for players" : L"");
		text[NUMBEROF(text) - 1] = 0;
		if (seconds > 0)
		{
			size_t length = ustrlen(text);

			usnprintf(text + length, NUMBEROF(text) - 1 - length, L" %d", seconds);
		}
		if (global_network_game_server_get() && p2p_invite_link(link, sizeof(link)))
		{
			size_t length = ustrlen(text);

			usnprintf(text + length, NUMBEROF(text) - 1 - length, L"\r\n\r\nInvite link copied:\r\npaste it to friends");
		}
		text_set_length(named(description, "lobby_game_data", 0), text, LOBBY_TEXT_LENGTH);
	}
	profile_name_show(description);
}

/* ---- an in-progress game's lobby, before joining it (Direct Link and
LAN's rows of games under way): what its advertisement tells (no players'
names: they come with joining), JOIN GAME */

/* the previewed game, found again among the client's (NULL: gone) */
static struct advertised_game *preview_game(void)
{
	void *client = global_network_game_client_get();
	struct advertised_game *games = client ? network_game_client_get_available_games(client) : NULL;
	short index;

	for (index = 0; games && index < MAXIMUM_ADVERTISED_GAMES; index++)
	{
		if (network_game_client_advertised_game_is_valid(&games[index]) &&
			!csmemcmp(games[index].key_id, multiplayer.preview_key_id, sizeof(multiplayer.preview_key_id)) &&
			!csmemcmp(games[index].xnaddr, multiplayer.preview_xnaddr, sizeof(multiplayer.preview_xnaddr)))
		{
			return &games[index];
		}
	}
	return NULL;
}

/* "port lobby preview update" */
static void preview_update(struct widget_instance *list)
{
	struct widget_instance *description = list->parameters.list.extended_description;
	struct advertised_game *game = preview_game();
	boolean valid = game != NULL;
	wchar_t text[LOBBY_TEXT_LENGTH], name[NUMBEROF(game->game_name) + 1];

	visible_set(named(description, "lobby_right_item", 0), valid);
	if (valid)
	{
		lobby_map_show(description, game->map_name);
		usnprintf(text, NUMBEROF(text) - 1, L"%s\r\n%d of %d players\r\non %d machines", engine_names[PIN(game->engine_type, 0, 5)],
			game->player_count, game->maximum_player_count, game->machine_count);
		text[NUMBEROF(text) - 1] = 0;
		text_set_length(named(description, "lobby_game_data", 0), text, LOBBY_TEXT_LENGTH);
		ustrncpy(name, game->game_name, NUMBEROF(game->game_name));
		name[NUMBEROF(game->game_name)] = 0;
		/* (the text box does not wrap: lines of up to 28 characters) */
		usnprintf(text, NUMBEROF(text) - 1, L"%s\r\n\r\nThis game is under way.\r\n\r\n%s", name,
			game->open ? L"JOIN GAME joins it now;\r\nits players show then." :
			L"It cannot be joined now:\r\nit is loading, over or full.");
	}
	else
		usnprintf(text, NUMBEROF(text) - 1, L"The game is gone.");
	text[NUMBEROF(text) - 1] = 0;
	text_set_length(named(list, "preview_status", 0), text, LOBBY_TEXT_LENGTH);
	profile_name_show(description);
}

/* "port lobby preview join" */
static boolean preview_join(struct widget_instance *widget, short controller, boolean *widget_deleted)
{
	struct advertised_game *game = preview_game();

	if (!game || !game->open)
		return campaign_fail();
	if (!multiplayer_player(controller))
		return FALSE;
	return ui_widget_port_join(widget, game, LOBBY_NAME, widget_deleted);
}

/* ---- the gametype editor: Edit Gametypes' list (the built-in gametypes
and those saved), the gametype being edited (player_ui's) or Server Setup's
(a copy of the game's gametype), its options' screens: each option a
spinner, found by its name (the PC version's screens put them in another
order than the Xbox's, whose functions of the same names walk them by
place) */

boolean ui_widget_port_gametype_edit_begin(long profile_index);
boolean ui_widget_port_gametype_save(struct widget_instance *widget, boolean *widget_deleted);
boolean ui_widget_port_game_variant_set(struct game_variant *variant, struct game_variant_options const *options);
boolean ui_widget_port_gametype_delete(long profile_index);

#define GAMETYPE_EDIT_ROWS 11

static struct
{
	/* the list's */
	long gametypes[MAXIMUM_GAMETYPES];
	short count, first, chosen;
	boolean stale;
	long deleting;
	/* Server Setup's copy of the game's gametype, edited in place of
	player_ui's (setup) */
	boolean setup;
	struct game_variant setup_variant;
	struct game_variant_options setup_options;
	/* the vehicle screen's side shown */
	short vehicle_side;
} gametype_edit = { { 0 }, 0, 0, 0, TRUE, NONE, FALSE };

static struct game_variant *edit_variant(void)
{
	return gametype_edit.setup ? &gametype_edit.setup_variant : player_ui_get_edit_playlist_profile();
}

static struct game_variant_options *edit_options(void)
{
	return gametype_edit.setup ? &gametype_edit.setup_options : player_ui_get_edit_playlist_options();
}

enum
{
	_option_long,		/* a long of the variant */
	_option_byte,		/* a byte of the variant */
	_option_flag,		/* a bit of the variant's flags (argument), set by value 1 */
	_option_health,		/* the variant's health, tenths */
	_option_short,		/* a short of the options */
	_option_option_byte,	/* a byte of the options */
	_option_radar		/* the options' radar players, and the variant's flag */
};

struct gametype_option
{
	char const *spinner;
	short kind;
	short offset;
	unsigned long argument;
	short count;
	long values[16];
};

#define VARIANT_FIELD(field) (short)offsetof(struct game_variant, field)
#define OPTIONS_FIELD(field) (short)offsetof(struct game_variant_options, field)
#define TIME_LIMITS { 0, 10, 15, 20, 25, 30, 45 }

static struct gametype_option const gametype_options[] =
{
	/* player options */
	{ "number_of_lives_spinner", _option_long, VARIANT_FIELD(universal_variant.lives), 0, 4, { 0, 1, 3, 5 } },
	{ "maximum_health_spinner", _option_health, 0, 0, 6, { 5, 10, 15, 20, 30, 40 } },
	{ "shields_spinner", _option_flag, 0, FLAG(_game_variant_no_shields_bit), 2, { 0, 1 } },
	{ "respawn_time_spinner", _option_long, VARIANT_FIELD(universal_variant.respawn_time), 0, 4, { 0, 150, 300, 450 } },
	{ "respawn_time_growth_spinner", _option_long, VARIANT_FIELD(universal_variant.respawn_time_growth), 0, 4,
		{ 0, 150, 300, 450 } },
	{ "odd_man_out_spinner", _option_byte, VARIANT_FIELD(universal_variant.odd_man_out), 0, 2, { 1, 0 } },
	{ "invisible_players_spinner", _option_flag, 0, FLAG(_game_variant_always_invisible_bit), 2, { 1, 0 } },
	{ "suicide_penalty_spinner", _option_long, VARIANT_FIELD(universal_variant.suicide_penalty), 0, 4,
		{ 0, 150, 300, 450 } },
	/* item options (weapon sets: the PC's list, then the Xbox's NO GRENADES) */
	{ "item_options_infinite_grenades_spinner", _option_flag, 0, FLAG(_game_variant_infinite_grenades_bit), 2,
		{ 1, 0 } },
	{ "item_options_weapon_set_spinner", _option_long, VARIANT_FIELD(universal_variant.weapon_set), 0, 14,
		{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 12, 13, 10 } },
	{ "item_options_starting_equipment_spinner", _option_flag, 0, FLAG(_game_variant_generic_starting_equipment_bit),
		2, { 0, 1 } },
	/* (the loadout: the weapon set's, or each player's two weapons) */
	{ "loadout_spinner", _option_option_byte, OPTIONS_FIELD(loadout), 0, 2, { _loadout_category, _loadout_custom } },
	{ "primary_weapon_spinner", _option_option_byte, OPTIONS_FIELD(primary_weapon), 0, NUMBER_OF_LOADOUT_WEAPONS,
		{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 } },
	{ "secondary_weapon_spinner", _option_option_byte, OPTIONS_FIELD(secondary_weapon), 0, NUMBER_OF_LOADOUT_WEAPONS,
		{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 } },
	/* indicator options */
	{ "indicator_options_radar display_spinner", _option_long, VARIANT_FIELD(universal_variant.goal_radar), 0, 3,
		{ 0, 1, 2 } },
	{ "indicator_options_players_on_radar_spinner", _option_radar, OPTIONS_FIELD(radar_players), 0, 3,
		{ _radar_players_all, _radar_players_friends, _radar_players_none } },
	{ "indicator_options_friends_on_screen_spinner", _option_flag, 0, FLAG(_game_variant_allow_friendly_navpoints_bit),
		2, { 1, 0 } },
	/* capture the flag */
	{ "assault_spinner", _option_byte, VARIANT_FIELD(game_engine_variant.ctf.assault), 0, 2, { 1, 0 } },
	{ "single_flag_spinner", _option_long, VARIANT_FIELD(game_engine_variant.ctf.single_flag_time), 0, 6,
		{ 0, 1800, 3600, 5400, 9000, 18000 } },
	{ "flag_must_reset_spinner", _option_byte, VARIANT_FIELD(game_engine_variant.ctf.flag_must_reset), 0, 2,
		{ 1, 0 } },
	{ "flag_at_home_spinner", _option_byte, VARIANT_FIELD(game_engine_variant.ctf.flag_at_home_to_score), 0, 2,
		{ 1, 0 } },
	{ "captures_to_win_spinner", _option_long, VARIANT_FIELD(universal_variant.score_to_win), 0, 5,
		{ 1, 3, 5, 10, 15 } },
	{ "time_limit_spinner", _option_short, OPTIONS_FIELD(time_limit), 0, 7, TIME_LIMITS },
	/* king of the hill */
	{ "koth_moving_hill_spinner", _option_byte, VARIANT_FIELD(game_engine_variant.king.moving_hill), 0, 2, { 1, 0 } },
	{ "koth_score_to_win_spinner", _option_long, VARIANT_FIELD(universal_variant.score_to_win), 0, 5,
		{ 1, 2, 5, 10, 15 } },
	{ "koth_team_play_spinner", _option_byte, VARIANT_FIELD(universal_variant.teams), 0, 2, { 1, 0 } },
	/* oddball */
	{ "trait_with_ball_spinner", _option_long, VARIANT_FIELD(game_engine_variant.oddball.trait_with_ball), 0, 4,
		{ 0, 1, 2, 3 } },
	{ "trait_without_ball_spinner", _option_long, VARIANT_FIELD(game_engine_variant.oddball.trait_without_ball), 0, 4,
		{ 0, 1, 2, 3 } },
	{ "speed_with_ball_spinner", _option_long, VARIANT_FIELD(game_engine_variant.oddball.speed_with_ball), 0, 3,
		{ 1, 0, 2 } },
	{ "ball_type_spinner", _option_long, VARIANT_FIELD(game_engine_variant.oddball.oddball_ball_type), 0, 3,
		{ 0, 1, 2 } },
	{ "random_start_spinner", _option_byte, VARIANT_FIELD(game_engine_variant.oddball.random_start), 0, 2, { 1, 0 } },
	{ "ball_spawn_count_spinner", _option_long, VARIANT_FIELD(game_engine_variant.oddball.ball_spawn_count), 0, 16,
		{ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 } },
	{ "score_to_win_spinner", _option_long, VARIANT_FIELD(universal_variant.score_to_win), 0, 5, { 1, 2, 5, 10, 15 } },
	/* (Slayer's, Oddball's and Race's) */
	{ "team_play_spinner", _option_byte, VARIANT_FIELD(universal_variant.teams), 0, 2, { 1, 0 } },
	/* race */
	{ "team_scoring_spinner", _option_long, VARIANT_FIELD(game_engine_variant.race.team_scoring), 0, 3, { 0, 1, 2 } },
	{ "race_type_spinner", _option_long, VARIANT_FIELD(game_engine_variant.race.race_type), 0, 3, { 0, 1, 2 } },
	{ "laps_to_win_spinner", _option_long, VARIANT_FIELD(universal_variant.score_to_win), 0, 6,
		{ 1, 3, 5, 10, 15, 25 } },
	/* slayer (the PC's rows' names and labels are crossed: kill_penalty's
	row is labelled KILL IN ORDER, kill_in_order's KILL PENALTY; each sets
	what its label says) */
	{ "death_bonus_spinner", _option_byte, VARIANT_FIELD(game_engine_variant.slayer.no_death_bonus), 0, 2, { 0, 1 } },
	{ "kill_penalty_spinner", _option_byte, VARIANT_FIELD(game_engine_variant.slayer.kill_in_order), 0, 2, { 1, 0 } },
	{ "kill_in_order_spinner", _option_byte, VARIANT_FIELD(game_engine_variant.slayer.no_kill_penalty), 0, 2,
		{ 0, 1 } },
	/* (up to 500, for big games: tools/port_settings.py's STRING_INSERTS) */
	{ "kills_to_win_spinner", _option_long, VARIANT_FIELD(universal_variant.score_to_win), 0, 11,
		{ 5, 10, 15, 25, 50, 75, 100, 150, 200, 250, 500 } },
	/* team options */
	{ "friendly_fire_spinner", _option_short, OPTIONS_FIELD(friendly_fire), 0, 4,
		{ _friendly_fire_off, _friendly_fire_on, _friendly_fire_shields_only, _friendly_fire_explosives_only } },
	{ "friendly_fire_penalty_spinner", _option_short, OPTIONS_FIELD(friendly_fire_penalty), 0, 4, { 0, 5, 10, 15 } },
	{ "autobalance_spinner", _option_option_byte, OPTIONS_FIELD(auto_team_balance), 0, 2, { 0, 1 } },
	/* vehicle options (the side's set and counts: vehicles_update) */
	{ "vehicles_respawn_spinner", _option_short, OPTIONS_FIELD(vehicle_respawn_time), 0, 7,
		{ 0, 30, 60, 90, 120, 180, 300 } },
};

/* (an instance's name is its definition's cut to 31 characters:
"item_options_infinite_grenades_") */
#define WIDGET_NAME_LENGTH 31

static struct gametype_option const *gametype_option_named(char const *spinner)
{
	short index;

	for (index = 0; index < NUMBEROF(gametype_options); index++)
	{
		if (!strncmp(gametype_options[index].spinner, spinner, WIDGET_NAME_LENGTH))
			return &gametype_options[index];
	}
	return NULL;
}

static long gametype_option_value(struct gametype_option const *option, struct game_variant *variant,
	struct game_variant_options *options)
{
	byte *v = (byte *)variant;
	byte *o = (byte *)options;

	switch (option->kind)
	{
	case _option_long: return *(long *)(v + option->offset);
	case _option_byte: return v[option->offset] != 0;
	case _option_flag: return (variant->universal_variant.flags & option->argument) != 0;
	case _option_health: return (long)(variant->universal_variant.health * 10.0f + 0.5f);
	case _option_short: return *(short *)(o + option->offset);
	case _option_option_byte: return o[option->offset];
	case _option_radar: return options->radar_players;
	}
	return 0;
}

static void gametype_option_value_set(struct gametype_option const *option, long value, struct game_variant *variant,
	struct game_variant_options *options)
{
	byte *v = (byte *)variant;
	byte *o = (byte *)options;

	switch (option->kind)
	{
	case _option_long: *(long *)(v + option->offset) = value; break;
	case _option_byte: v[option->offset] = (byte)value; break;
	case _option_flag:
		if (value)
			variant->universal_variant.flags |= option->argument;
		else
			variant->universal_variant.flags &= ~option->argument;
		break;
	case _option_health: variant->universal_variant.health = (real)value / 10.0f; break;
	case _option_short: *(short *)(o + option->offset) = (short)value; break;
	case _option_option_byte: o[option->offset] = (byte)value; break;
	case _option_radar:
		/* (and the Xbox's flag: other players on the tracker or not) */
		options->radar_players = (byte)value;
		if (value == _radar_players_none)
			variant->universal_variant.flags &= ~FLAG(_game_variant_draw_object_in_motion_sensor_bit);
		else
			variant->universal_variant.flags |= FLAG(_game_variant_draw_object_in_motion_sensor_bit);
		break;
	}
}

/* the value's place in the option's list: its own, else the nearest */
static short gametype_option_index(struct gametype_option const *option, long value)
{
	short index, best = 0;
	long best_distance = 0x7FFFFFFF;

	for (index = 0; index < option->count; index++)
	{
		long distance = option->values[index] > value ? option->values[index] - value : value - option->values[index];

		if (distance < best_distance)
		{
			best_distance = distance;
			best = index;
		}
	}
	return best;
}

/* each of the screen's option rows' spinner, with the option */
static void gametype_options_each(struct widget_instance *list, boolean save)
{
	struct game_variant *variant = edit_variant();
	struct game_variant_options *options = edit_options();
	struct widget_instance *row;

	if (!variant || !options)
		return;
	for (row = list->child; row; row = row->next)
	{
		struct widget_instance *spinner;

		for (spinner = row->child; spinner; spinner = spinner->next)
		{
			/* (a spinner: its label's name, cut to 31 characters, can be
			the same as its own) */
			struct gametype_option const *option = spinner->type == 2 ? gametype_option_named(spinner->name) : NULL;

			if (!option)
				continue;
			if (save)
			{
				short index = (short)PIN(spinner->parameters.list.selected_index, 0, option->count - 1);

				gametype_option_value_set(option, option->values[index], variant, options);
			}
			else
				spinner->parameters.list.selected_index =
					gametype_option_index(option, gametype_option_value(option, variant, options));
		}
	}
}

/* the options' list of a button of its bar (OK's): the one above it whose
rows are its options */
static struct widget_instance *gametype_options_list(struct widget_instance *widget)
{
	struct widget_instance *list;

	for (list = widget; list; list = list->parent)
	{
		struct widget_instance *child;

		for (child = list->child; child; child = child->next)
		{
			if (!strncmp(child->name, "op_", 3))
				return list;
		}
	}
	return NULL;
}

/* ---- the vehicle options: a side's (red's, blue's) set and counts, on
the spinners while its side is shown */

static char const *const vehicle_spinners[NUMBER_OF_VARIANT_VEHICLES] =
{
	"warthog_spinner", "ghost_spinner", "scorpion_spinner", "rwarthog_spinner", "banshee_spinner", "cgturret_spinner"
};
/* (the presets' spinner: the vehicle sets 0 to 7, then CUSTOM) */
#define VEHICLE_PRESET_CUSTOM 8

static struct widget_instance *vehicle_spinner(struct widget_instance *list, char const *name)
{
	struct widget_instance *row;

	for (row = list->child; row; row = row->next)
	{
		struct widget_instance *spinner = named(row, name, 0);

		if (spinner)
			return spinner;
	}
	return NULL;
}

static void vehicles_show(struct widget_instance *list)
{
	struct game_variant_options *options = edit_options();
	struct widget_instance *spinner;
	short side = gametype_edit.vehicle_side, index;

	if (!options)
		return;
	if ((spinner = vehicle_spinner(list, "team_spinner")) != NULL)
		spinner->parameters.list.selected_index = side;
	if ((spinner = vehicle_spinner(list, "vehicle_presets_spinner")) != NULL)
		spinner->parameters.list.selected_index = options->vehicle_set[side] == VARIANT_VEHICLE_SET_CUSTOM ?
			VEHICLE_PRESET_CUSTOM : (short)MIN(options->vehicle_set[side], VEHICLE_PRESET_CUSTOM - 1);
	for (index = 0; index < NUMBER_OF_VARIANT_VEHICLES; index++)
	{
		if ((spinner = vehicle_spinner(list, vehicle_spinners[index])) != NULL)
			spinner->parameters.list.selected_index =
				(short)MIN(options->vehicle_counts[side][index], MAXIMUM_VARIANT_VEHICLE_COUNT);
	}
}

/* the side shown's spinners kept (and the Xbox's vehicle set: red's, if it
is one of the Xbox's) */
static void vehicles_keep(struct widget_instance *list)
{
	struct game_variant *variant = edit_variant();
	struct game_variant_options *options = edit_options();
	struct widget_instance *spinner;
	short side = gametype_edit.vehicle_side, index;

	if (!options || !variant)
		return;
	if ((spinner = vehicle_spinner(list, "vehicle_presets_spinner")) != NULL)
		options->vehicle_set[side] = spinner->parameters.list.selected_index >= VEHICLE_PRESET_CUSTOM ?
			VARIANT_VEHICLE_SET_CUSTOM : (byte)spinner->parameters.list.selected_index;
	for (index = 0; index < NUMBER_OF_VARIANT_VEHICLES; index++)
	{
		if ((spinner = vehicle_spinner(list, vehicle_spinners[index])) != NULL)
			options->vehicle_counts[side][index] =
				(byte)PIN(spinner->parameters.list.selected_index, 0, MAXIMUM_VARIANT_VEHICLE_COUNT);
	}
	variant->universal_variant.vehicle_set = options->vehicle_set[0] <= 4 ? options->vehicle_set[0] : 0;
}

/* "mp prof vehicles update": the side changed (the one left kept, the other
shown); a count changed makes the side's set CUSTOM */
static void vehicles_update(struct widget_instance *list)
{
	struct game_variant_options *options = edit_options();
	struct widget_instance *team = vehicle_spinner(list, "team_spinner");
	struct widget_instance *preset = vehicle_spinner(list, "vehicle_presets_spinner");
	short index;

	if (!options)
		return;
	if (team && team->parameters.list.selected_index != gametype_edit.vehicle_side)
	{
		vehicles_keep(list);
		gametype_edit.vehicle_side = (short)PIN(team->parameters.list.selected_index, 0, 1);
		vehicles_show(list);
		return;
	}
	for (index = 0; preset && index < NUMBER_OF_VARIANT_VEHICLES; index++)
	{
		struct widget_instance *spinner = vehicle_spinner(list, vehicle_spinners[index]);

		if (spinner && spinner->parameters.list.selected_index !=
			options->vehicle_counts[gametype_edit.vehicle_side][index])
		{
			options->vehicle_counts[gametype_edit.vehicle_side][index] = (byte)spinner->parameters.list.selected_index;
			preset->parameters.list.selected_index = VEHICLE_PRESET_CUSTOM;
		}
	}
}

/* "mp profile init X" (the list's creation): its spinners from the gametype */
static boolean gametype_options_init(struct widget_instance *list)
{
	if (!edit_variant())
		return campaign_fail();
	gametype_options_each(list, FALSE);
	if (named(list, "op_team", 0))
		vehicles_show(list);
	return TRUE;
}

/* "mp profile set X" (OK): the gametype from its spinners */
static boolean gametype_options_save(struct widget_instance *widget)
{
	struct widget_instance *list = gametype_options_list(widget);

	if (!list || !edit_variant())
		return campaign_fail();
	if (named(list, "op_team", 0))
		vehicles_keep(list);
	gametype_options_each(list, TRUE);
	return TRUE;
}

/* ---- Server Setup's options: a copy of the game's gametype (the one chosen
before), edited by the editor's screens, the game's on START GAME */

static void gametype_setup_begin(void)
{
	if (gametype_edit.setup)
		return;
	if (!player_ui_game_variant_specified(&gametype_edit.setup_variant))
		return;
	gametype_edit.setup_options = *player_ui_get_game_variant_options();
	gametype_edit.setup = TRUE;
	gametype_edit.vehicle_side = 0;
}

static void gametype_setup_end(void)
{
	gametype_edit.setup = FALSE;
}

static boolean gametype_setup_apply(void)
{
	boolean applied = !gametype_edit.setup ||
		ui_widget_port_game_variant_set(&gametype_edit.setup_variant, &gametype_edit.setup_options);

	gametype_edit.setup = FALSE;
	return applied;
}

/* the game type row's: the gametype's name and type */
static void gametype_setup_type(wchar_t *text)
{
	wchar_t name[NUMBEROF(gametype_edit.setup_variant.human_readable_game_description) + 1];

	text[0] = 0;
	if (!gametype_edit.setup)
		return;
	ustrncpy(name, gametype_edit.setup_variant.human_readable_game_description, NUMBEROF(name) - 1);
	name[NUMBEROF(name) - 1] = 0;
	usnprintf(text, ROW_TEXT_LENGTH - 1, L"%s (%s%s)", name,
		engine_names[PIN(gametype_edit.setup_variant.game_engine_index, 0, 5)],
		gametype_edit.setup_variant.universal_variant.teams ? L", TEAMS" : L"");
	text[ROW_TEXT_LENGTH - 1] = 0;
}

/* "port setup edit" (a Server Setup option's row): its screen edits the copy */
static boolean gametype_setup_edit(void)
{
	gametype_setup_begin();
	return gametype_edit.setup || campaign_fail();
}

/* ---- Edit Gametypes' list */

static void gametype_edit_read(void)
{
	short last;

	gametype_edit.count = ui_widget_port_gametypes(gametype_edit.gametypes, MAXIMUM_GAMETYPES, &last);
	gametype_edit.chosen = (short)MIN(gametype_edit.chosen, MAX(0, gametype_edit.count - 1));
	gametype_edit.stale = FALSE;
}

/* "mp profiles list initialize" (Edit Gametypes'): on the gametype used last */
static boolean gametype_edit_list_initialize(struct widget_instance *list)
{
	short last;

	gametype_edit.setup = FALSE;
	gametype_edit.count = ui_widget_port_gametypes(gametype_edit.gametypes, MAXIMUM_GAMETYPES, &last);
	gametype_edit.chosen = last;
	gametype_edit.first = (short)MAX(0, MIN(last - GAMETYPE_EDIT_ROWS / 2, gametype_edit.count - GAMETYPE_EDIT_ROWS));
	gametype_edit.stale = FALSE;
	focus_row(list, (short)(gametype_edit.chosen - gametype_edit.first));
	return TRUE;
}

static void gametype_edit_row_text(short row, wchar_t *text)
{
	gametype_display_name(gametype_edit.gametypes[gametype_edit.first + row], text);
}

/* "gt edit list update": the rows (scrolled), the gametype chosen's
picture, name and rules */
static void gametype_edit_list_update(struct widget_instance *list)
{
	struct widget_instance *description = list->parameters.list.extended_description;
	struct widget_instance *widget;
	short focused;

	if (gametype_edit.stale)
		gametype_edit_read();
	focused = list_scroll(list, &gametype_edit.first, gametype_edit.count, GAMETYPE_EDIT_ROWS);
	if (focused != NONE)
		gametype_edit.chosen = (short)MIN(focused, gametype_edit.count - 1);
	rows_update(list, (short)MIN(gametype_edit.count, GAMETYPE_EDIT_ROWS), gametype_edit_row_text);
	visible_set(named(description, "gametype_right_item", 0), gametype_edit.count > 0);
	if (gametype_edit.chosen < gametype_edit.count)
	{
		long profile_index = gametype_edit.gametypes[gametype_edit.chosen];
		struct game_variant variant;
		wchar_t text[ROW_TEXT_LENGTH * 2];

		visible_set(named(description, "locked_gametype_icon", 0),
			((unsigned long)profile_index & PLAYLIST_READ_ONLY_BIT) != 0);
		gametype_display_name(profile_index, text);
		text_set(named(description, "gametype_right_name", 0), text);
		if (playlist_profile_get(profile_index, &variant))
		{
			long engine = PIN(variant.game_engine_index, 0, 5);

			if ((widget = named(description, "gametype_right_pic", 0)) != NULL)
				widget->animation.current_frame_index = (short)engine;
			usnprintf(text, NUMBEROF(text) - 1, L"%s\r\n%s\r\nScore to win: %ld", engine_names[engine],
				variant.universal_variant.teams ? L"Teams" : L"Free for all", variant.universal_variant.score_to_win);
			text[NUMBEROF(text) - 1] = 0;
			text_set(named(description, "gametype_right_data", 0), text);
		}
	}
	profile_name_show(description);
}

/* "mp profile begin editing" (the list's custom activation: OK, a row's A) */
static boolean gametype_edit_begin(void)
{
	if (gametype_edit.chosen >= gametype_edit.count)
		return campaign_fail();
	gametype_edit.setup = FALSE;
	gametype_edit.vehicle_side = 0;
	return ui_widget_port_gametype_edit_begin(gametype_edit.gametypes[gametype_edit.chosen]);
}

/* "request del playlist profile" (X, DELETE): not a built-in one */
static boolean gametype_delete_request(void)
{
	long profile_index = gametype_edit.chosen < gametype_edit.count ? gametype_edit.gametypes[gametype_edit.chosen] :
		NONE;

	if (profile_index == NONE || ((unsigned long)profile_index & PLAYLIST_READ_ONLY_BIT))
		return campaign_fail();
	gametype_edit.deleting = profile_index;
	return TRUE;
}

/* "final del playlist profile" */
static boolean gametype_delete_final(void)
{
	boolean deleted = gametype_edit.deleting != NONE && ui_widget_port_gametype_delete(gametype_edit.deleting);

	gametype_edit.deleting = NONE;
	gametype_edit.stale = TRUE;
	return deleted;
}

/* ---- the gametype's screen and the gametype type's */

/* "get edit game settings name": the gametype's name */
static void gametype_edit_name(struct widget_instance *text_box)
{
	struct game_variant *variant = edit_variant();
	wchar_t name[NUMBEROF(variant->human_readable_game_description) + 1];

	if (!variant)
		return;
	ustrncpy(name, variant->human_readable_game_description, NUMBEROF(name) - 1);
	name[NUMBEROF(name) - 1] = 0;
	text_set(text_box, name);
}

/* "game settings lists text update": the help of the option with the
focus, for its value (the description's strings are each option's values'
helps in turn), as the Xbox's, which stops the game on the buttons' row */
static void gametype_option_help(struct widget_instance *list)
{
	struct widget_instance *description = list->parameters.list.extended_description;
	struct widget_instance *row;
	short index = 0;

	/* (Item Options: the weapon set's row for a category loadout, the two
	weapons' for a custom one) */
	if (named(list, "loadout_spinner", 0))
	{
		boolean custom = named(list, "loadout_spinner", 0)->parameters.list.selected_index == _loadout_custom;

		visible_set(named(list, "op_weapon_set", 0), !custom);
		visible_set(named(list, "op_primary_weapon", 0), custom);
		visible_set(named(list, "op_secondary_weapon", 0), custom);
	}
	/* (the server browser's filters, hidden: their helps are fewer than
	their values) */
	if (!description || !list->focused_child || !strncmp(list->name, "filters", 7))
		return;
	for (row = list->child; row; row = row->next)
	{
		struct widget_instance *spinner;

		for (spinner = row->child; spinner && spinner->type != 2 /* spinner */; spinner = spinner->next)
			;
		if (row == list->focused_child)
		{
			if (spinner)
				description->parameters.text_box.string_list_index = (short)(index + spinner->parameters.list.selected_index);
			return;
		}
		if (spinner)
			index += spinner->parameters.list.number_of_items;
	}
}

static char const *const engine_items[] =
{
	/* (the type list's rows, in order: game_engine_ctf and so on) */
	"gametype_select_ctf_item", "gametype_select_koth_item", "gametype_select_slayer_item",
	"gametype_select_oddball_item", "gametype_select_race_item"
};
static long const engine_of_item[] = { 1, 4, 2, 3, 5 };

/* "mp profile init game engine": the gametype's type focused */
static boolean gametype_engine_init(struct widget_instance *list)
{
	struct game_variant *variant = edit_variant();
	short index;

	if (!variant)
		return campaign_fail();
	for (index = 0; index < NUMBEROF(engine_items); index++)
	{
		if (engine_of_item[index] == variant->game_engine_index)
			focus_row(list, index);
	}
	return TRUE;
}

/* "mp profile set game engine" (a type's A): another type clears the type's
own rules (as the Xbox's) */
static boolean gametype_engine_set(struct widget_instance *item)
{
	struct game_variant *variant = edit_variant();
	short index;

	if (!variant)
		return campaign_fail();
	for (index = 0; index < NUMBEROF(engine_items); index++)
	{
		if (!strcmp(item->name, engine_items[index]))
		{
			if (variant->game_engine_index != engine_of_item[index])
				csmemset(&variant->game_engine_variant, 0, sizeof(variant->game_engine_variant));
			variant->game_engine_index = engine_of_item[index];
			return TRUE;
		}
	}
	return campaign_fail();
}

/* "mp edit profile set rule text": the gametype's type (its string in
ui\multiplayer_game_text, as the Xbox's: capture the flag 3, slayer 4,
oddball 5, king of the hill 6, race 7, unknown 8) */
static void gametype_engine_name(struct widget_instance *text_box)
{
	static short const strings[] = { 8, 3, 4, 5, 6, 7 };
	struct game_variant *variant = edit_variant();

	if (variant)
		text_box->parameters.text_box.string_list_index = strings[PIN(variant->game_engine_index, 0, 5)];
}

/* ---------- public code */

boolean pc_menu_event_function_invoke(
	struct widget_instance *widget,
	struct event_record *event,
	long function_index,
	boolean *widget_deleted)
{
	short controller = event_controller(widget, event);

	switch (function_index)
	{
	case _pc_menu_function_quit_game:
		platform_request_quit();
		return TRUE;
	case _pc_menu_function_setting_load:
		return setting_load(widget);
	case _pc_menu_function_setting_save:
		return setting_save(widget);
	}
	{
		char const *name = pc_menu_function_name(function_index);

		if (!name)
			return FALSE;
		if (!strcmp(name, "main menu quit game"))
		{
			platform_request_quit();
		}
		else if (!strcmp(name, "profile set edit begin"))
		{
			return pc_menu_profile_edit_begin();
		}
		else if (!strcmp(name, "mouse emit accept event"))
		{
			event_manager_post_button(controller_of(widget), BUTTON_A);
		}
		else if (!strcmp(name, "mouse emit back event"))
		{
			event_manager_post_button(controller_of(widget), BUTTON_B);
		}
		else if (!strcmp(name, "mouse emit x event"))
		{
			event_manager_post_button(controller_of(widget), BUTTON_X);
		}
		else if (!strcmp(name, "emit custom activation event"))
		{
			struct widget_instance *top = widget;

			while (top->parent)
				top = top->parent;
			boolean deleted;

			ui_widget_port_dispatch_event(top, EVENT_CUSTOM_ACTIVATION, event_controller(widget, event), &deleted);
			if (deleted)
				*widget_deleted = TRUE;
		}
		else if (!strcmp(name, "mp type set mode"))
		{
			lan_mode = strstr(widget->name, "_lan_") != NULL;
			multiplayer_mode_set(widget);
			/* (Create's: the map list only for a game made, "join controller
			to mp game" before; it fails with the game's port in use, by
			another copy of the game) */
			if ((multiplayer.mode == _multiplayer_mode_host_internet ||
				multiplayer.mode == _multiplayer_mode_host_lan) && !global_network_game_server_get())
			{
				platform_log("menus: the game could not be made (is its port in use?)");
				return campaign_fail();
			}
		}
		else if (!strcmp(name, "gamespy screen init"))
		{
			return browser_initialize(widget, event, widget_deleted);
		}
		else if (!strcmp(name, "gamespy select item") || !strcmp(name, "gamespy select button"))
		{
			return browser_select(widget, event, controller, widget_deleted);
		}
		else if (!strcmp(name, "gamespy back handler") && text_field_editing(NULL))
		{
			text_field_end(FALSE);
		}
		else if (!strcmp(name, "port setup edit"))
		{
			return gametype_setup_edit();
		}
		else if (!strcmp(name, "mp profile save changes"))
		{
			return ui_widget_port_gametype_save(widget, widget_deleted);
		}
		else if (!strcmp(name, "mp profile begin editing"))
		{
			return gametype_edit_begin();
		}
		else if (!strcmp(name, "request del playlist profile"))
		{
			return gametype_delete_request();
		}
		else if (!strcmp(name, "final del playlist profile"))
		{
			return gametype_delete_final();
		}
		else if (!strcmp(name, "mp profile init game engine"))
		{
			return gametype_engine_init(widget);
		}
		else if (!strcmp(name, "mp profile set game engine"))
		{
			return gametype_engine_set(widget);
		}
		else if (!strncmp(name, "mp profile init ", 16) || !strcmp(name, "mp prof init teamplay options") ||
			!strcmp(name, "mp prof init vehicle options"))
		{
			return gametype_options_init(widget);
		}
		else if ((!strncmp(name, "mp profile set ", 15) && strcmp(name, "mp profile set for game")) ||
			!strcmp(name, "mp prof save teamplay options") || !strcmp(name, "mp prof save vehicle options"))
		{
			return gametype_options_save(widget);
		}
		else if (!strcmp(name, "port lobby preview join"))
		{
			return preview_join(widget, controller, widget_deleted);
		}
		else if (!strcmp(name, "direct ip connect go"))
		{
			return direct_link_from_clipboard();
		}
		else if (!strcmp(name, "join controller to mp game"))
		{
			return multiplayer_host(widget, event, controller, widget_deleted);
		}
		else if (!strcmp(name, "mp level list initialize"))
		{
			return map_list_initialize(widget);
		}
		else if (!strcmp(name, "mp level select"))
		{
			return ui_widget_port_multiplayer_map_choose(multiplayer.map_chosen);
		}
		else if (!strcmp(name, "mp profiles list initialize"))
		{
			if (!strcmp(widget->name, "playlist_select_list"))
				return gametype_edit_list_initialize(widget);
			return gametype_list_initialize(widget);
		}
		else if (!strcmp(name, "mp profile set for game"))
		{
			return gametype_choose();
		}
		else if (!strcmp(name, "server settings init"))
		{
			return server_settings_initialize(widget);
		}
		else if (!strcmp(name, "ss edit server name"))
		{
			return server_name_edit(widget);
		}
		else if (!strcmp(name, "ss copy invite"))
		{
			return invite_copy();
		}
		else if (!strcmp(name, "ss start game"))
		{
			return server_start();
		}
		else if (!strcmp(name, "single prev cl item activated"))
		{
			item_activated(widget, event_controller(widget, event), widget_deleted);
		}
		else if (!strcmp(name, "port settings save"))
		{
			settings_each(screen_of(widget), setting_changed_save);
			platform_display_apply();
		}
		else if (!strcmp(name, "port settings defaults"))
		{
			settings_each(screen_of(widget), setting_default_show);
		}
		else if (!strcmp(name, "controls screen init") || !strcmp(name, "controls screen defaults"))
		{
			return controls_load(!strcmp(name, "controls screen defaults"));
		}
		else if (!strcmp(name, "controls screen change set"))
		{
			return controls_save();
		}
		else if (!strcmp(name, "controls begin binding"))
		{
			return control_capture_begin(widget);
		}
		else if (!strcmp(name, "player profile list initialize"))
		{
			return profile_list_initialize(widget);
		}
		else if (!strcmp(name, "profile manager select"))
		{
			return profile_choose(controller);
		}
		else if (!strcmp(name, "request del player profile"))
		{
			return profile_delete_request();
		}
		else if (!strcmp(name, "final del player profile"))
		{
			return profile_delete();
		}
		else if (!strcmp(name, "color picker menu initialize"))
		{
			return color_list_initialize(widget);
		}
		else if (!strcmp(name, "color picker select color"))
		{
			return color_choose();
		}
		else if (!strcmp(name, "controls binding slot"))
		{
			controls_screen.slot = (short)!controls_screen.slot;
		}
		else if (!strcmp(name, "campaign menu init"))
		{
			return campaign_menu_initialize(controller);
		}
		else if (!strcmp(name, "campaign menu continue"))
		{
			return campaign_continue(controller);
		}
		else if (!strcmp(name, "initialize sp level list solo"))
		{
			return level_list_initialize(widget, controller);
		}
		else if (!strcmp(name, "solo level set map"))
		{
			return level_choose();
		}
		else if (!strcmp(name, "difficulty item select"))
		{
			return difficulty_start(sibling_index(widget), controller);
		}
		else if (!strcmp(name, "set difficulty"))
		{
			main_set_difficulty(PIN(difficulty_shown(widget), 0, 3));
		}
		else if (!strcmp(name, "load game menu init"))
		{
			return saved_game_list_initialize(widget, controller);
		}
		else if (!strcmp(name, "load game menu activated"))
		{
			return saved_game_continue(controller);
		}
		else if (!strcmp(name, "load game menu delete request"))
		{
			return saved_game_delete_request();
		}
		else if (!strcmp(name, "load game menu delete finish"))
		{
			return saved_game_delete(controller);
		}
		else if (!strcmp(name, "controls back handler") || !strcmp(name, "gamespy back handler") ||
			!strcmp(name, "gamespy dismiss error") || !strcmp(name, "gamespy dismiss filters"))
		{
			ui_widget_port_go_back(widget);
			*widget_deleted = TRUE;
		}
		return TRUE;
	}
}

void pc_menu_game_data_function_invoke(
	struct widget_instance *widget,
	long function)
{
	char const *name = pc_menu_game_data_input_name(function);

	if (!name)
		return;
	/* (a text field whose screen has gone: let go of) */
	if (text_field.row && system_milliseconds() - text_field_shown_time > 500)
		text_field_end(FALSE);
	if (!strcmp(name, "solo map list update"))
		level_list_update(widget);
	else if (!strcmp(name, "mp map list update"))
		map_list_update(widget);
	else if (!strcmp(name, "gt select list update"))
		gametype_list_update(widget);
	else if (!strcmp(name, "server settings update"))
		server_settings_update(widget);
	else if (!strcmp(name, "gamespy screen update"))
		browser_update(widget);
	else if (!strcmp(name, "gt edit list update"))
		gametype_edit_list_update(widget);
	else if (!strcmp(name, "get edit game settings name"))
		gametype_edit_name(widget);
	else if (!strcmp(name, "game settings lists text update"))
		gametype_option_help(widget);
	else if (!strcmp(name, "mp edit profile set rule text"))
		gametype_engine_name(widget);
	else if (!strcmp(name, "mp prof vehicles update"))
		vehicles_update(widget);
	else if (!strcmp(name, "port lobby preview update"))
		preview_update(widget);
	else if (!strcmp(name, "port lobby update"))
		lobby_update(widget);
	else if (!strcmp(name, "port settings help"))
		settings_help(widget);
	else if (!strcmp(name, "controls update menu"))
		controls_update(widget);
	else if (!strcmp(name, "color picker update"))
		color_list_update(widget);
	else if (!strcmp(name, "3wide player profile list update"))
		profile_list_update(widget);
	else if (!strcmp(name, "load game list update"))
		saved_game_list_update(widget);
}
