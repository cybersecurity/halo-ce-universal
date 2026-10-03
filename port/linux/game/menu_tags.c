/*
MENU_TAGS.C

The menus written in XML (halo_menus.h, read by port/linux/src/menu_files.c),
built into the tags the game's widgets are made of when ui.map has loaded
(scenario_tags_load), and added to its tag table: a widget definition
('DeLa') for each <widget>, a unicode string list ('ustr') for each
<strings> and each widget's own text, and a bitmap group ('bitm') for each
<bitmap>, whose textures are drawn from their PNGs (menu_art_texture,
xbox_textures.c). The game's widgets then run them as their own:
main_screen_shell_load opens pc_menus_root_name()'s.

Each name the files use is looked up here: our widgets (named pc\<name>),
the map's tags (a path, with its backslashes), the game's event handler and
game data functions, and the port's own (menu_functions.c, from
PC_MENU_FUNCTION_BASE), which include the PC version's (most of which do
nothing yet). Its mouse events are kept, though this engine never sends
them (the port's mouse support clicks with A). Anything that does not resolve, or that the widget
code would stop the game for (a spinner's children, a list from strings
with children), is logged with its file and line and nothing is added: the
game keeps its own menus.

The tag table cannot grow where it is (the tags' names follow it), so it
is copied, with ours after it; every existing tag keeps its index. All of it
is let go in scenario_tags_unload, before the next map's tags load.
*/

#include "cseries.h"
#include "bitmaps/bitmap_group.h"
#include "tag_files/tag_groups.h"
#include "text/text_group.h"
#include "rasterizer/xbox/rasterizer_xbox_hardware_bitmaps.h"

#include "halo_menus.h"

#include <stdlib.h>
#include <string.h>

/* the platform layer's (port/linux/src) */
void platform_log(char const *format, ...);
char const *config_string(char const *name);

/* cache_files.c's (port) */
void *cache_files_tag_instances(long *count);
void cache_files_set_tag_instances(void *instances, long count);

/* the game's (port) */
char *tag_get_name(long tag_index);
char const *ui_widget_event_handler_function_name(long function_index);

void menu_tags_loaded(char const *map_name);
void menu_tags_unloaded(void);
char const *pc_menus_root_name(void);
char const *pc_menu_function_name(long function_index);
char const *pc_menu_game_data_input_name(long function_index);
boolean pc_menu_tag(long tag_index);

/* ---------- constants */

#define PC_MENU_TAG_PREFIX "pc\\"
#define UI_WIDGET_DEFINITION_TAG 'DeLa'
#define UNICODE_STRING_LIST_TAG 'ustr'
#define FONT_TAG 'font'
#define SOUND_TAG 'snd!'
#define MAXIMUM_STRINGS 64
#define MAXIMUM_TEXT 2048
/* the size of a menu bitmap's D3D texture, which only stands for its PNG */
#define PLACEHOLDER_SIZE 4

enum
{
	_bitmap_type_2d = 0,
	_bitmap_format_a8r8g8b8 = 11,
	_bitmap_has_power_of_two_dimensions_bit = 0,
};

enum
{
	_event_handler_close_current_widget_bit = 0,
	_event_handler_close_other_widget_bit,
	_event_handler_close_all_widgets_bit,
	_event_handler_open_widget_bit,
	_event_handler_reload_self_bit,
	_event_handler_reload_other_widget_bit,
	_event_handler_give_focus_to_widget_bit,
	_event_handler_run_function_bit,
	_event_handler_replace_self_with_widget_bit,
	_event_handler_go_back_to_previous_widget_bit,
	_event_handler_run_scenario_script_bit,
	_event_handler_try_to_branch_on_failure_bit,
};

enum
{
	_widget_type_container,
	_widget_type_text_box,
	_widget_type_spinner_list,
	_widget_type_column_list,
};

enum
{
	_list_items_generated_from_string_list_tag_bit = 1,
};

enum
{
	_child_widget_use_custom_controller_index_bit = 0,
};

/* ---------- structures */

struct cache_file_tag_instance
{
	long group_tag;
	long parent_group_tags[2];
	long tag_index;
	char *name;
	void *base_address;
	unsigned long unused[2];
};

typedef char verify_cache_file_tag_instance_size[
	sizeof(struct cache_file_tag_instance) == 0x20 ? 1 : -1];

/* the widget definition, as ui_widget.c has it */
struct ui_widget_event_handler_reference
{
	long flags;
	short event_type;
	short function;
	struct tag_reference widget_tag;
	struct tag_reference sound_effect;
	char script[32];
};

struct ui_widget_child_reference
{
	struct tag_reference widget_tag;
	char name[32];
	long flags;
	short custom_controller_index;
	short vertical_offset;
	short horizontal_offset;
	byte unknown03A[0x50 - 0x3A];
};

struct ui_widget_conditional_reference
{
	struct tag_reference widget_tag;
	char name[32];
	long flags;
	short custom_controller_index;
	byte unknown036[0x50 - 0x36];
};

struct ui_widget_search_and_replace_reference
{
	char search_string[32];
	short replace_function;
};

struct ui_widget_game_data_input_reference
{
	short function;
	byte unknown002[0x24 - 0x02];
};

struct ui_widget_definition
{
	short type;
	short controller_index;
	char name[32];
	rectangle2d bounds;
	long flags;
	long milliseconds_to_auto_close;
	long auto_close_fade_time;
	struct tag_reference background_bitmap;
	struct tag_block game_data_inputs;
	struct tag_block event_handlers;
	struct tag_block search_and_replace_functions;
	byte unknown06C[0xEC - 0x6C];
	struct tag_reference text_label_string_list;
	struct tag_reference text_font;
	real_argb_color text_color;
	short justification;
	word text_box_flags;
	byte unknown120[0x12E - 0x120];
	short string_list_index;
	short horizontal_offset;
	short vertical_offset;
	byte unknown134[0x150 - 0x134];
	long list_flags;
	struct tag_reference list_header_bitmap;
	struct tag_reference list_footer_bitmap;
	rectangle2d list_header_bounds;
	rectangle2d list_footer_bounds;
	byte unknown184[0x1A4 - 0x184];
	struct tag_reference extended_description_widget;
	byte unknown1B4[0x2D4 - 0x1B4];
	struct tag_block conditional_widgets;
	byte unknown2E0[0x3E0 - 0x2E0];
	struct tag_block child_widgets;
};

typedef char verify_ui_widget_event_handler_reference_size[
	sizeof(struct ui_widget_event_handler_reference) == 0x48 ? 1 : -1];
typedef char verify_ui_widget_child_reference_size[
	sizeof(struct ui_widget_child_reference) == 0x50 ? 1 : -1];
typedef char verify_ui_widget_conditional_reference_size[
	sizeof(struct ui_widget_conditional_reference) == 0x50 ? 1 : -1];
typedef char verify_ui_widget_search_and_replace_reference_size[
	sizeof(struct ui_widget_search_and_replace_reference) == 0x22 ? 1 : -1];
typedef char verify_ui_widget_game_data_input_reference_size[
	sizeof(struct ui_widget_game_data_input_reference) == 0x24 ? 1 : -1];
typedef char verify_ui_widget_definition_text_color_offset[
	offsetof(struct ui_widget_definition, text_color) == 0x10C ? 1 : -1];
typedef char verify_ui_widget_definition_list_flags_offset[
	offsetof(struct ui_widget_definition, list_flags) == 0x150 ? 1 : -1];
typedef char verify_ui_widget_definition_conditional_widgets_offset[
	offsetof(struct ui_widget_definition, conditional_widgets) == 0x2D4 ? 1 : -1];
typedef char verify_ui_widget_definition_size[
	sizeof(struct ui_widget_definition) == 0x3EC ? 1 : -1];

/* a spinner bound to a setting (menu_functions.c) */
struct pc_menu_setting
{
	long definition_index;
	char const *setting;
	long value_count;
	char const *values[MAXIMUM_STRINGS];
	short loaded_index;
};

/* ---------- globals */

/* the names the files may use, by their values */
static char const *const event_names[] =
{
	"a", "b", "x", "y", "black", "white", "left_trigger", "right_trigger",
	"up", "down", "left", "right", "start", "back", "left_thumb", "right_thumb",
	"stick_up", "stick_down", "stick_left", "stick_right",
	"right_stick_up", "right_stick_down", "right_stick_left", "right_stick_right",
	"created", "deleted",
	/* (the PC version's, which this engine never sends) */
	"get_focus", "lose_focus", "left_mouse", "middle_mouse", "right_mouse", "double_click", "custom_activation",
	"post_render",
};

static char const *const widget_flag_names[] =
{
	"pass_unhandled_to_focused_child", "pause_game", "flash_bitmap", "up_down_tabs_children",
	"left_right_tabs_children", "up_down_tabs_items", "left_right_tabs_items", "no_focused_child",
	"pass_unhandled_to_all_children", "render_any_controller", "pass_handled_to_all_children",
	"main_menu_if_no_history", "tag_controller_index", "nifty_fx", "no_history",
	/* (the PC version's, which this engine has no use for) */
	"force_handle_mouse", "no_widescreen_fill",
};

static char const *const widget_type_names[] = { "container", "text", "spinner", "column_list" };
static char const *const list_flag_names[] = { "items_in_code", "items_from_strings", "one_tooltip", "single_preview" };
static char const *const text_flag_names[] = { "editable", "password", "flashing", "no_focus_test" };
static char const *const align_names[] = { "left", "right", "center" };
/* (build_number and pid are the PC version's, which this engine has not) */
static char const *const replace_names[] = { "none", "controller", "build_number", "pid" };

/* the game data functions, as the tags name them
(ui_widget_game_data_input_functions.c) */
static char const *const game_data_input_names[] =
{
	"NULL", "player settings menu update desc", "unused", "playlist settings menu update desc",
	"gametype select menu update desc", "multiplayer type menu update desc", "solo level select update",
	"difficulty menu update desc", "build number textbox only", "server list update",
	"network pregame status update", "splitscreen pregame status update", "net splitscreen prejoin players",
	"mp profile list update", "3wide player profile list update", "plyr prof edit select menu upd8",
	"player profile small menu update", "game settings lists text update", "solo game objective text",
	"color picker update", "game settings lists pic update", "main menu fake animate",
	"mp level select update", "get active plyr profile name", "get edit plyr profile name",
	"get edit game settings name", "get active plyr profile color", "mp set textbox map name",
	"mp set textbox game ruleset", "mp set textbox teams noteams", "mp set textbox score limit",
	"mp set textbox score limit type", "mp set bitmap for map", "mp set bitmap for ruleset",
	"mp set textbox", "mp edit profile set rule text", "system link status check", "mp game directions",
	"teams no teams bitmap update", "warn if diff will nuke saved game", "dim if no net cable",
};

/* the port's event handler functions (menu_functions.c), from
PC_MENU_FUNCTION_BASE: its own, then the PC version's that the Xbox's have
not (as its tags name them), then its own of the Xbox's names, which our
widgets run instead of the Xbox's (those take the Xbox's widgets: a spinner
of levels, the difficulty list itself) */
static char const *const port_function_names[] =
{
	"port quit game", "port setting load", "port setting save", "unwired",
	"exit gracefully to xbox dashboard", "pause game invert pitch", "start new coop game",
	"pause game invert spinner get", "pause game invert spinner set", "main menu quit game",
	"mouse emit accept event", "mouse emit back event", "mouse emit dpad left event",
	"mouse emit dpad right event", "mouse spinner 3wide click", "controls screen init", "video screen init",
	"controls begin binding", "gamespy screen init", "gamespy screen dispose", "gamespy select header",
	"gamespy select item", "gamespy select button", "plr prof init mouse set", "plr prof change mouse set",
	"plr prof init audio set", "plr prof change audio set", "plr prof change video set",
	"controls screen dispose", "controls screen change set", "mouse emit x event", "gamepad screen init",
	"gamepad screen dispose", "gamepad screen change gamepads", "gamepad screen select item",
	"mouse screen defaults", "audio screen defaults", "video screen defaults", "controls screen defaults",
	"profile set edit begin", "profile manager delete", "profile manager select", "gamespy dismiss error",
	"server settings init", "ss edit server name", "ss edit server password", "ss start game",
	"video test dialog init", "video test dialog dispose", "video test dialog accept",
	"gamespy dismiss filters", "gamespy update filter settings", "gamespy back handler",
	"mouse spinner 1wide click", "controls back handler", "controls advanced launch", "controls advanced ok",
	"mp pause menu open", "mp game options open", "mp choose team", "mp prof init vehicle options",
	"mp prof save vehicle options", "single prev cl item activated", "mp prof init teamplay options",
	"mp prof save teamplay options", "mp game options choose", "emit custom activation event",
	"plr prof cancel audio set", "plr prof init network options", "plr prof save network options",
	"difficulty item select",
	"gamespy get patch", "video screen dispose", "campaign menu init", "campaign menu continue",
	"load game menu init", "load game menu dispose", "load game menu activated", "solo menu save checkpoint",
	"mp type set mode", "direct ip connect init",
	"direct ip connect go", "direct ip edit field", "network settings edit a port", "network settings defaults",
	"load game menu delete request", "load game menu delete finish",
	"initialize sp level list solo", "dispose sp level list", "solo level set map", "set difficulty",
	"port settings save", "port settings defaults", "controls binding slot",
	"color picker menu initialize", "color picker menu dispose", "color picker select color",
	"player profile list initialize", "player profile list dispose", "request del player profile",
	"final del player profile",
	"ss copy invite", "join controller to mp game", "mp level list initialize", "mp level list dispose",
	"mp level select", "mp profiles list initialize", "mp profiles list dispose", "mp profile set for game",
	"port lobby preview join",
	"port setup edit",
	/* (the gametype editor's: the Xbox's walk their rows by place, which the
	PC version's screens changed) */
	"mp profile begin editing", "mp profile save changes", "request del playlist profile", "final del playlist profile",
	"mp profile init game engine", "mp profile set game engine", "mp profile init ctf rules",
	"mp profile init koth rules", "mp profile init slayer rules", "mp profile init oddball rules",
	"mp profile init racing rules", "mp profile init player opts", "mp profile init item options",
	"mp profile init indicator opts", "mp profile set ctf rules", "mp profile set koth rules",
	"mp profile set slayer rules", "mp profile set oddball rules", "mp profile set racing rules",
	"mp profile set player options", "mp profile set item options", "mp profile set indicator opts",
};

/* the PC version's game data functions that the Xbox's have not, from
PC_MENU_FUNCTION_BASE (menu_functions.c) */
static char const *const port_game_data_input_names[] =
{
	"unwired",
	"pause game set textbox inverted", "dim unless two controllers", "controls update menu",
	"video menu update", "gamespy screen update", "common button bar update", "gamepad update menu",
	"server settings update", "audio menu update", "mp prof vehicles update", "solo map list update",
	"mp map list update", "gt select list update", "gt edit list update", "load game list update",
	"direct ip connect update", "network settings update",
	"port settings help", "color picker update", "3wide player profile list update", "port lobby update",
	"port lobby preview update",
	/* (the gametype editor's: the Xbox's stops the game on the buttons'
	row; the Xbox's read only player_ui's gametype, not Server Setup's) */
	"game settings lists text update", "get edit game settings name", "mp edit profile set rule text",
};

static struct
{
	/* what is let go when the map unloads */
	void **blocks;
	long block_count;
	struct bitmap_data **bitmaps;
	long bitmap_count;
	struct cache_file_tag_instance *original_instances;
	long original_count;
	struct pc_menu_setting *settings;
	long setting_count;
	boolean loaded;
	char root[300];
} menu_tags;

/* the build under way */
static struct
{
	struct halo_menus const *menus;
	long first_index;
	long first_salt;
	long next;
	/* each widget's, bitmap's and string list's tag index, and each
	widget's own text's and strings' (NONE: none) */
	long *widget_tags;
	long *bitmap_tags;
	long *strings_tags;
	long *text_tags;
	long *spinner_tags;
	boolean failed;
} build;

/* ---------- private code */

/* (the game's allocator, which malloc and free are here: cseries.h) */
static void *allocate(long size)
{
	void *block = malloc(size > 0 ? size : 1);

	if (!block)
	{
		build.failed = TRUE;
		return NULL;
	}
	memset(block, 0, size > 0 ? size : 1);
	menu_tags.blocks = realloc(menu_tags.blocks, (menu_tags.block_count + 1) * sizeof(*menu_tags.blocks));
	menu_tags.blocks[menu_tags.block_count++] = block;
	return block;
}

static void problem(char const *file, long line, char const *message, char const *detail)
{
	if (!build.failed)
		halo_menus_log(file, line, message, detail);
	build.failed = TRUE;
}

static long name_index(char const *name, char const *const *names, long count)
{
	long index;

	for (index = 0; name && index < count; index++)
	{
		if (!strcmp(name, names[index]))
			return index;
	}
	return NONE;
}

/* the bits of the space-separated names */
static long flags_parse(char const *text, char const *const *names, long count, char const *file, long line)
{
	long flags = 0;

	while (text && *text)
	{
		char word[64];
		long length, bit;

		text += strspn(text, " \t\r\n");
		length = (long)strcspn(text, " \t\r\n");
		if (!length)
			break;
		if (length >= (long)sizeof(word))
			length = sizeof(word) - 1;
		memcpy(word, text, length);
		word[length] = 0;
		text += strcspn(text, " \t\r\n");
		bit = name_index(word, names, count);
		if (bit == NONE)
			problem(file, line, "there is no flag", word);
		else
			flags |= 1L << bit;
	}
	return flags;
}

static void reference_clear(struct tag_reference *reference, long group_tag)
{
	reference->group_tag = group_tag;
	reference->name = "";
	reference->name_length = 0;
	reference->index = NONE;
}

static void reference_set(struct tag_reference *reference, long group_tag, long tag_index)
{
	if (tag_index == NONE)
	{
		reference_clear(reference, group_tag);
		return;
	}
	reference->group_tag = group_tag;
	reference->name = (char *)tag_get_name(tag_index);
	reference->name_length = (long)strlen(reference->name);
	reference->index = tag_index;
}

static long find(char const *name, long count, char const *(*name_of)(long))
{
	long index;

	for (index = 0; name && index < count; index++)
	{
		if (!strcmp(name_of(index), name))
			return index;
	}
	return NONE;
}

static char const *widget_name(long index) { return build.menus->widgets[index].name; }
static char const *bitmap_name(long index) { return build.menus->bitmaps[index].name; }
static char const *strings_name(long index) { return build.menus->string_lists[index].name; }

static long widget_named(char const *name)
{
	return find(name, build.menus->widget_count, widget_name);
}

static long map_tag(long group_tag, char const *name, char const *file, long line)
{
	long index = tag_loaded(group_tag, name);

	if (index == NONE)
		problem(file, line, "the map has no tag", name);
	return index;
}

/* a tag: the map's by its path (with a backslash), else one of ours */
static long tag_named(long group_tag, char const *name, char const *file, long line)
{
	long index = NONE;

	if (strchr(name, '\\'))
		return map_tag(group_tag, name, file, line);
	switch (group_tag)
	{
	case UI_WIDGET_DEFINITION_TAG:
		index = widget_named(name);
		index = index == NONE ? NONE : build.widget_tags[index];
		break;
	case BITMAP_GROUP_TAG:
		index = find(name, build.menus->bitmap_count, bitmap_name);
		index = index == NONE ? NONE : build.bitmap_tags[index];
		break;
	case UNICODE_STRING_LIST_TAG:
		index = find(name, build.menus->string_list_count, strings_name);
		index = index == NONE ? NONE : build.strings_tags[index];
		break;
	}
	if (index == NONE)
		problem(file, line, "there is nothing named", name);
	return index;
}

/* "unwired <name>": a function the PC version has for these widgets, not
yet written for this engine (port/assets/menus/UNWIRED.md), which does
nothing */
static boolean unwired(char const *name)
{
	return !strncmp(name, "unwired ", 8);
}

static long function_index(char const *name, char const *file, long line)
{
	char const *function_name;
	long index;

	if (unwired(name))
		name = "unwired";
	index = name_index(name, port_function_names, NUMBEROF(port_function_names));
	if (index != NONE)
		return PC_MENU_FUNCTION_BASE + index;
	for (index = 0; (function_name = ui_widget_event_handler_function_name(index)) != NULL; index++)
	{
		if (!strcmp(name, function_name))
			return index;
	}
	problem(file, line, "there is no event handler function", name);
	return NONE;
}

static long game_data_input_index(char const *name, char const *file, long line)
{
	long index;

	if (unwired(name))
		name = "unwired";
	/* (ours first, as for the event handlers) */
	index = name_index(name, port_game_data_input_names, NUMBEROF(port_game_data_input_names));
	if (index != NONE)
		return PC_MENU_FUNCTION_BASE + index;
	index = name_index(name, game_data_input_names, NUMBEROF(game_data_input_names));
	if (index != NONE)
		return index;
	problem(file, line, "there is no game data input", name);
	return NONE;
}

static long hex_digit(char character)
{
	if (character >= '0' && character <= '9')
		return character - '0';
	if (character >= 'a' && character <= 'f')
		return character - 'a' + 10;
	if (character >= 'A' && character <= 'F')
		return character - 'A' + 10;
	return NONE;
}

/* "#RRGGBB" or "#AARRGGBB" */
static boolean parse_color(char const *text, real_argb_color *color)
{
	long length = text ? (long)strlen(text) : 0;
	long components[4] = { 255, 0, 0, 0 };
	long index;

	if ((length != 7 && length != 9) || text[0] != '#')
		return FALSE;
	for (index = 0; index < (length - 1) / 2; index++)
	{
		long high = hex_digit(text[1 + 2 * index]), low = hex_digit(text[2 + 2 * index]);

		if (high == NONE || low == NONE)
			return FALSE;
		components[length == 7 ? index + 1 : index] = high * 16 + low;
	}
	color->alpha = components[0] / 255.0f;
	color->red = components[1] / 255.0f;
	color->green = components[2] / 255.0f;
	color->blue = components[3] / 255.0f;
	return TRUE;
}

/* "top left bottom right" */
static void parse_bounds(char const *text, rectangle2d *bounds, char const *file, long line)
{
	long top, left, bottom, right;
	char extra;

	if (text && sscanf(text, "%ld %ld %ld %ld %c", &top, &left, &bottom, &right, &extra) == 4)
	{
		bounds->y0 = (short)top;
		bounds->x0 = (short)left;
		bounds->y1 = (short)bottom;
		bounds->x1 = (short)right;
	}
	else if (text)
	{
		problem(file, line, "bounds are \"top left bottom right\":", text);
	}
}

/* a 'ustr' of the strings */
static void *string_list_build(char const *const *strings, long count)
{
	struct string_list *list = allocate(sizeof(struct string_list));
	struct string_list_entry *entries = allocate(count * sizeof(struct string_list_entry));
	long index;

	if (!list || !entries)
		return list;
	list->strings.count = count;
	list->strings.address = entries;
	for (index = 0; index < count; index++)
	{
		long length = (long)strlen(strings[index]);
		unsigned short *characters = allocate((length * 2 + 2) * sizeof(unsigned short));
		long written;

		if (!characters)
			return list;
		written = halo_menus_utf16(strings[index], characters, length * 2 + 2);
		entries[index].string.size = written * (long)sizeof(unsigned short);
		entries[index].string.address = characters;
	}
	return list;
}

/* the "|"-separated pieces of text, copied (at most MAXIMUM_STRINGS) */
static long split(char const *text, char const **pieces)
{
	long count = 0;

	while (text && count < MAXIMUM_STRINGS)
	{
		long length = (long)strcspn(text, "|");
		char *piece = allocate(length + 1);

		if (!piece)
			break;
		memcpy(piece, text, length);
		pieces[count++] = piece;
		if (!text[length])
			break;
		text += length + 1;
	}
	return count;
}

static void *bitmap_build(struct halo_menu_bitmap const *source, long tag_index)
{
	struct bitmap_group *group = allocate(sizeof(struct bitmap_group));
	struct bitmap_group_sequence *sequence = allocate(sizeof(struct bitmap_group_sequence));
	struct bitmap_data *bitmaps = allocate(source->frame_count * sizeof(struct bitmap_data));
	long frame;

	if (build.failed)
		return group;
	if (!source->frame_count)
	{
		problem(source->file, source->line, "a bitmap has no frames:", source->name);
		return group;
	}
	sequence->first_bitmap_index = 0;
	sequence->bitmap_count = (short)source->frame_count;
	group->sequences.count = 1;
	group->sequences.address = sequence;
	group->bitmaps.count = source->frame_count;
	group->bitmaps.address = bitmaps;
	for (frame = 0; frame < source->frame_count; frame++)
	{
		struct halo_menu_frame const *data = &build.menus->frames[source->first_frame + frame];
		struct bitmap_data *bitmap = &bitmaps[frame];

		/* (a frame of the map's: a copy of its bitmap, which the texture cache
		loads from the map as its own) */
		if (data->map)
		{
			long group_index = map_tag(BITMAP_GROUP_TAG, data->map, source->file, source->line);
			struct bitmap_group *group_source;

			if (group_index == NONE)
				return group;
			group_source = bitmap_group_get(group_index);
			if (data->index < 0 || data->index >= group_source->bitmaps.count)
			{
				problem(source->file, source->line, "the map's bitmap has no such frame:", data->map);
				return group;
			}
			memcpy(bitmap, (struct bitmap_data *)group_source->bitmaps.address + data->index, sizeof(*bitmap));
			bitmap->cache_block_index = NONE;
			bitmap->base_address = NULL;
			continue;
		}

		if (data->width <= 0 || data->height <= 0 || data->width > 2048 || data->height > 2048)
		{
			problem(source->file, source->line, "a frame is 1 to 2048 wide and high:", data->png);
			return group;
		}
		/* (its D3D texture is a small one, which its PNG is drawn for: the
		game draws a bitmap in units of its size, to whatever texture it has) */
		bitmap->signature = BITMAP_GROUP_TAG;
		bitmap->width = PLACEHOLDER_SIZE;
		bitmap->height = PLACEHOLDER_SIZE;
		bitmap->depth = 1;
		bitmap->type = _bitmap_type_2d;
		bitmap->format = _bitmap_format_a8r8g8b8;
		bitmap->flags = FLAG(_bitmap_has_power_of_two_dimensions_bit);
		bitmap->tag_index = tag_index;
		bitmap->cache_block_index = NONE;
		if (!rasterizer_bitmap_new(bitmap))
		{
			problem(source->file, source->line, "could not make the texture of bitmap", source->name);
			return group;
		}
		bitmap->width = (short)data->width;
		bitmap->height = (short)data->height;
		menu_tags.bitmaps = realloc(menu_tags.bitmaps, (menu_tags.bitmap_count + 1) * sizeof(*menu_tags.bitmaps));
		menu_tags.bitmaps[menu_tags.bitmap_count++] = bitmap;
		/* (none without a renderer: debug.null_renderer) */
		if (bitmap->hardware_format)
			halo_menus_art_register(bitmap->hardware_format, data->png);
	}
	return group;
}

/* adds a conditional widget to the definition, if it has not got it */
static void conditional_add(struct ui_widget_definition *definition, long tag_index, long flags)
{
	struct ui_widget_conditional_reference *conditionals =
		(struct ui_widget_conditional_reference *)definition->conditional_widgets.address;
	struct ui_widget_conditional_reference *grown;
	long count = definition->conditional_widgets.count, index;

	for (index = 0; index < count; index++)
	{
		if (conditionals[index].widget_tag.index == tag_index)
			return;
	}
	grown = allocate((count + 1) * sizeof(*grown));
	if (!grown)
		return;
	/* (the game's memcpy asserts on a NULL source, even for nothing) */
	if (count)
		memcpy(grown, conditionals, count * sizeof(*grown));
	reference_set(&grown[count].widget_tag, UI_WIDGET_DEFINITION_TAG, tag_index);
	grown[count].flags = flags;
	definition->conditional_widgets.address = grown;
	definition->conditional_widgets.count = count + 1;
}

static void handler_build(struct ui_widget_event_handler_reference *handler, struct halo_menu_handler const *source,
	long event_type, struct ui_widget_definition *definition)
{
	char const *targets[5];
	long target_count = 0;

	handler->event_type = (short)event_type;
	reference_clear(&handler->widget_tag, UI_WIDGET_DEFINITION_TAG);
	reference_clear(&handler->sound_effect, SOUND_TAG);
	if (source->run)
	{
		handler->function = (short)function_index(source->run, source->file, source->line);
		SET_FLAG(handler->flags, _event_handler_run_function_bit, TRUE);
	}
	if (source->script)
	{
		strncpy(handler->script, source->script, sizeof(handler->script) - 1);
		SET_FLAG(handler->flags, _event_handler_run_scenario_script_bit, TRUE);
	}
	else if (source->label)
	{
		strncpy(handler->script, source->label, sizeof(handler->script) - 1);
	}
	if (source->open)
	{
		targets[target_count++] = source->open;
		SET_FLAG(handler->flags, _event_handler_open_widget_bit, TRUE);
	}
	if (source->replace)
	{
		targets[target_count++] = source->replace;
		SET_FLAG(handler->flags, _event_handler_replace_self_with_widget_bit, TRUE);
	}
	if (source->focus)
	{
		targets[target_count++] = source->focus;
		SET_FLAG(handler->flags, _event_handler_give_focus_to_widget_bit, TRUE);
	}
	if (source->back)
		SET_FLAG(handler->flags, _event_handler_go_back_to_previous_widget_bit, TRUE);
	if (source->branch)
		SET_FLAG(handler->flags, _event_handler_try_to_branch_on_failure_bit, TRUE);
	if (source->close)
	{
		if (!strcmp(source->close, "current"))
			SET_FLAG(handler->flags, _event_handler_close_current_widget_bit, TRUE);
		else if (!strcmp(source->close, "all"))
			SET_FLAG(handler->flags, _event_handler_close_all_widgets_bit, TRUE);
		else if (!strcmp(source->close, "other") && source->widget)
			SET_FLAG(handler->flags, _event_handler_close_other_widget_bit, TRUE);
		else
			problem(source->file, source->line, "close is \"current\", \"all\" or \"other\" (with widget=)",
				source->close);
	}
	if (source->reload)
	{
		if (!strcmp(source->reload, "self"))
			SET_FLAG(handler->flags, _event_handler_reload_self_bit, TRUE);
		else if (!strcmp(source->reload, "other") && source->widget)
			SET_FLAG(handler->flags, _event_handler_reload_other_widget_bit, TRUE);
		else
			problem(source->file, source->line, "reload is \"self\" or \"other\" (with widget=)", source->reload);
	}
	if (source->widget)
		targets[target_count++] = source->widget;
	if (target_count > 1)
	{
		long index;

		/* (several flags may share the one widget) */
		for (index = 1; index < target_count; index++)
		{
			if (strcmp(targets[index], targets[0]))
				problem(source->file, source->line, "a handler names one widget (open, replace, focus or widget)",
					NULL);
		}
	}
	if (target_count)
	{
		reference_set(&handler->widget_tag, UI_WIDGET_DEFINITION_TAG,
			tag_named(UI_WIDGET_DEFINITION_TAG, targets[0], source->file, source->line));
	}
	if (source->sound)
		reference_set(&handler->sound_effect, SOUND_TAG, map_tag(SOUND_TAG, source->sound, source->file, source->line));
	if (source->otherwise)
	{
		if (!source->run)
			problem(source->file, source->line, "otherwise needs run (the function whose failure it is for)", NULL);
		SET_FLAG(handler->flags, _event_handler_try_to_branch_on_failure_bit, TRUE);
		conditional_add(definition, tag_named(UI_WIDGET_DEFINITION_TAG, source->otherwise, source->file, source->line), 1);
	}
}

static void setting_add(struct halo_menu_widget const *source, long definition_index)
{
	struct pc_menu_setting *setting;

	menu_tags.settings = realloc(menu_tags.settings, (menu_tags.setting_count + 1) * sizeof(*menu_tags.settings));
	setting = &menu_tags.settings[menu_tags.setting_count++];
	memset(setting, 0, sizeof(*setting));
	setting->definition_index = definition_index;
	setting->setting = source->setting;
	setting->loaded_index = NONE;
	setting->value_count = split(source->values, setting->values);
}

/* a font: large, small, terminal, or the map's by its path */
static long font_tag(char const *font, char const *file, long line)
{
	char const *path = !strcmp(font, "large") ? "ui\\large_ui" :
		!strcmp(font, "small") ? "ui\\small_ui" :
		!strcmp(font, "terminal") ? "ui\\interstate" : font;

	return map_tag(FONT_TAG, path, file, line);
}

static void *widget_build(long widget_index)
{
	struct halo_menus const *menus = build.menus;
	struct halo_menu_widget const *source = &menus->widgets[widget_index];
	struct ui_widget_definition *definition = allocate(sizeof(struct ui_widget_definition));
	char const *leaf = strrchr(source->name, '/') ? strrchr(source->name, '/') + 1 : source->name;
	long child, handler, input, conditional, replace, count;

	if (!definition)
		return NULL;
	strncpy(definition->name, leaf, sizeof(definition->name) - 1);
	definition->type = _widget_type_container;
	if (source->type)
	{
		long type = name_index(source->type, widget_type_names, NUMBEROF(widget_type_names));

		if (type == NONE)
			problem(source->file, source->line, "there is no widget type", source->type);
		else
			definition->type = (short)type;
	}
	definition->controller_index = 4;
	if (source->controller && strcmp(source->controller, "any"))
	{
		if (strlen(source->controller) != 1 || source->controller[0] < '1' || source->controller[0] > '4')
			problem(source->file, source->line, "controller is 1 to 4, or \"any\":", source->controller);
		else
			definition->controller_index = (short)(source->controller[0] - '1');
	}
	definition->bounds.y0 = (short)source->top;
	definition->bounds.x0 = (short)source->left;
	definition->bounds.y1 = (short)(source->top + (source->has_height ? source->height : 480));
	definition->bounds.x1 = (short)(source->left + (source->has_width ? source->width : 640));
	definition->flags = flags_parse(source->flags, widget_flag_names, NUMBEROF(widget_flag_names),
		source->file, source->line);
	definition->milliseconds_to_auto_close = source->auto_close;
	definition->auto_close_fade_time = source->auto_close_fade;
	reference_clear(&definition->background_bitmap, BITMAP_GROUP_TAG);
	if (source->bitmap)
	{
		reference_set(&definition->background_bitmap, BITMAP_GROUP_TAG,
			tag_named(BITMAP_GROUP_TAG, source->bitmap, source->file, source->line));
	}
	/* the game data inputs */
	for (count = 0, input = source->first_input; input != HALO_MENU_NONE; input = menus->inputs[input].next)
		count++;
	if (count)
	{
		struct ui_widget_game_data_input_reference *inputs = allocate(count * sizeof(*inputs));

		definition->game_data_inputs.count = count;
		definition->game_data_inputs.address = inputs;
		for (count = 0, input = source->first_input; inputs && input != HALO_MENU_NONE; input = menus->inputs[input].next)
		{
			struct halo_menu_input const *data = &menus->inputs[input];

			inputs[count++].function = (short)game_data_input_index(data->input, data->file, data->line);
		}
	}
	/* search and replace */
	for (count = 0, replace = source->first_replace; replace != HALO_MENU_NONE; replace = menus->replaces[replace].next)
		count++;
	if (count)
	{
		struct ui_widget_search_and_replace_reference *replaces = allocate(count * sizeof(*replaces));

		definition->search_and_replace_functions.count = count;
		definition->search_and_replace_functions.address = replaces;
		for (count = 0, replace = source->first_replace; replaces && replace != HALO_MENU_NONE;
			replace = menus->replaces[replace].next)
		{
			struct halo_menu_replace const *item = &menus->replaces[replace];
			long function = item->function ? name_index(item->function, replace_names, NUMBEROF(replace_names)) : 0;

			if (function == NONE)
				problem(item->file, item->line, "a replace function is none or controller:", item->function);
			strncpy(replaces[count].search_string, item->search, sizeof(replaces[count].search_string) - 1);
			/* (the PC version's own are none here) */
			replaces[count++].replace_function = (short)(function == 1 ? 1 : 0);
		}
	}
	/* the text */
	reference_clear(&definition->text_label_string_list, UNICODE_STRING_LIST_TAG);
	reference_clear(&definition->text_font, FONT_TAG);
	if ((source->text != NULL) + (source->strings != NULL) + (source->string_list != NULL) > 1)
		problem(source->file, source->line, "a widget has one of text, strings and string_list:", source->name);
	if (build.text_tags[widget_index] != NONE)
	{
		reference_set(&definition->text_label_string_list, UNICODE_STRING_LIST_TAG, build.text_tags[widget_index]);
	}
	else if (build.spinner_tags[widget_index] != NONE)
	{
		reference_set(&definition->text_label_string_list, UNICODE_STRING_LIST_TAG, build.spinner_tags[widget_index]);
		definition->list_flags |= FLAG(_list_items_generated_from_string_list_tag_bit);
	}
	else if (source->string_list)
	{
		reference_set(&definition->text_label_string_list, UNICODE_STRING_LIST_TAG,
			tag_named(UNICODE_STRING_LIST_TAG, source->string_list, source->file, source->line));
	}
	definition->string_list_index = (short)source->string_index;
	/* (our own text is white in the large font, unless given) */
	if (source->text || source->strings)
	{
		definition->text_color.alpha = definition->text_color.red = 1.0f;
		definition->text_color.green = definition->text_color.blue = 1.0f;
		if (!source->font)
			reference_set(&definition->text_font, FONT_TAG, font_tag("large", source->file, source->line));
	}
	if (source->color && !parse_color(source->color, &definition->text_color))
		problem(source->file, source->line, "a color is #RRGGBB or #AARRGGBB:", source->color);
	if (source->font)
		reference_set(&definition->text_font, FONT_TAG, font_tag(source->font, source->file, source->line));
	if (source->align)
	{
		long align = name_index(source->align, align_names, NUMBEROF(align_names));

		if (align == NONE)
			problem(source->file, source->line, "align is \"left\", \"right\" or \"center\":", source->align);
		else
			definition->justification = (short)align;
	}
	definition->text_box_flags = (word)flags_parse(source->text_flags, text_flag_names, NUMBEROF(text_flag_names),
		source->file, source->line);
	definition->horizontal_offset = (short)source->text_x;
	definition->vertical_offset = (short)source->text_y;
	/* the list */
	definition->list_flags |= flags_parse(source->list_flags, list_flag_names, NUMBEROF(list_flag_names),
		source->file, source->line);
	if (source->strings && (definition->type != _widget_type_spinner_list || source->first_child != HALO_MENU_NONE))
		problem(source->file, source->line, "strings are for a spinner with no children:", source->name);
	if (TEST_FLAG(definition->list_flags, _list_items_generated_from_string_list_tag_bit) &&
		(definition->type != _widget_type_spinner_list || source->first_child != HALO_MENU_NONE ||
			definition->text_label_string_list.index == NONE))
		problem(source->file, source->line, "items_from_strings is for a spinner with strings and no children:",
			source->name);
	if (source->setting)
	{
		if (!source->strings || !source->values)
			problem(source->file, source->line, "a setting needs strings and values:", source->name);
		else
			setting_add(source, build.widget_tags[widget_index]);
	}
	reference_clear(&definition->list_header_bitmap, BITMAP_GROUP_TAG);
	reference_clear(&definition->list_footer_bitmap, BITMAP_GROUP_TAG);
	if (source->header_bitmap)
	{
		reference_set(&definition->list_header_bitmap, BITMAP_GROUP_TAG,
			tag_named(BITMAP_GROUP_TAG, source->header_bitmap, source->file, source->line));
	}
	if (source->footer_bitmap)
	{
		reference_set(&definition->list_footer_bitmap, BITMAP_GROUP_TAG,
			tag_named(BITMAP_GROUP_TAG, source->footer_bitmap, source->file, source->line));
	}
	parse_bounds(source->header_bounds, &definition->list_header_bounds, source->file, source->line);
	parse_bounds(source->footer_bounds, &definition->list_footer_bounds, source->file, source->line);
	reference_clear(&definition->extended_description_widget, UI_WIDGET_DEFINITION_TAG);
	if (source->description)
	{
		if (definition->type != _widget_type_column_list)
			problem(source->file, source->line, "a description is for a column list:", source->name);
		reference_set(&definition->extended_description_widget, UI_WIDGET_DEFINITION_TAG,
			tag_named(UI_WIDGET_DEFINITION_TAG, source->description, source->file, source->line));
	}
	/* the children */
	for (count = 0, child = source->first_child; child != HALO_MENU_NONE; child = menus->children[child].next)
		count++;
	if (definition->type == _widget_type_spinner_list && count != 0 && count != 1 && count != 3)
		problem(source->file, source->line, "a spinner has 0, 1 or 3 children:", source->name);
	if (count)
	{
		struct ui_widget_child_reference *children = allocate(count * sizeof(*children));

		definition->child_widgets.count = count;
		definition->child_widgets.address = children;
		for (count = 0, child = source->first_child; children && child != HALO_MENU_NONE; child = menus->children[child].next)
		{
			struct halo_menu_child const *entry = &menus->children[child];
			long tag_index = entry->nested != HALO_MENU_NONE ? build.widget_tags[entry->nested] :
				tag_named(UI_WIDGET_DEFINITION_TAG, entry->widget, entry->file, entry->line);
			char const *name = entry->nested != HALO_MENU_NONE ? menus->widgets[entry->nested].name : entry->widget;
			char const *child_leaf = strrchr(name, '/') ? strrchr(name, '/') + 1 : name;

			reference_set(&children[count].widget_tag, UI_WIDGET_DEFINITION_TAG, tag_index);
			strncpy(children[count].name, child_leaf, sizeof(children[count].name) - 1);
			children[count].horizontal_offset = (short)entry->x;
			children[count].vertical_offset = (short)entry->y;
			if (entry->controller)
			{
				if (strlen(entry->controller) != 1 || entry->controller[0] < '1' || entry->controller[0] > '4')
					problem(entry->file, entry->line, "a child's controller is 1 to 4:", entry->controller);
				SET_FLAG(children[count].flags, _child_widget_use_custom_controller_index_bit, TRUE);
				children[count].custom_controller_index = (short)(entry->controller[0] - '1');
			}
			count++;
		}
	}
	/* the conditional widgets */
	for (conditional = source->first_conditional; conditional != HALO_MENU_NONE;
		conditional = menus->conditionals[conditional].next)
	{
		struct halo_menu_conditional const *item = &menus->conditionals[conditional];

		conditional_add(definition, tag_named(UI_WIDGET_DEFINITION_TAG, item->widget, item->file, item->line),
			item->if_failed ? 1 : 0);
	}
	/* the event handlers: one for each of each <on>'s events */
	for (count = 0, handler = source->first_handler; handler != HALO_MENU_NONE; handler = menus->handlers[handler].next)
		count++;
	if (count)
	{
		long total = 0;

		/* (count the events) */
		for (handler = source->first_handler; handler != HALO_MENU_NONE; handler = menus->handlers[handler].next)
		{
			char const *text = menus->handlers[handler].event;

			while (text && *text)
			{
				long length;

				text += strspn(text, " \t\r\n");
				length = (long)strcspn(text, " \t\r\n");
				if (!length)
					break;
				total++;
				text += length;
			}
		}
		if (total)
		{
			struct ui_widget_event_handler_reference *handlers = allocate(total * sizeof(*handlers));

			definition->event_handlers.count = total;
			definition->event_handlers.address = handlers;
			for (count = 0, handler = source->first_handler; handlers && handler != HALO_MENU_NONE;
				handler = menus->handlers[handler].next)
			{
				struct halo_menu_handler const *on = &menus->handlers[handler];
				char const *text = on->event;

				while (text && *text)
				{
					char word[64];
					long length, event;

					text += strspn(text, " \t\r\n");
					length = (long)strcspn(text, " \t\r\n");
					if (!length)
						break;
					if (length >= (long)sizeof(word))
						length = sizeof(word) - 1;
					memcpy(word, text, length);
					word[length] = 0;
					text += strcspn(text, " \t\r\n");
					event = name_index(word, event_names, NUMBEROF(event_names));
					if (event == NONE)
						problem(on->file, on->line, "there is no event", word);
					handler_build(&handlers[count++], on, event, definition);
				}
			}
		}
	}
	return definition;
}

/* the tag table with count more entries, the existing ones as they are */
static struct cache_file_tag_instance *instances_grow(long count, long *first_index, long *first_salt)
{
	long existing, index, salt = 0;
	struct cache_file_tag_instance *instances = cache_files_tag_instances(&existing);
	struct cache_file_tag_instance *grown;

	if (!instances)
		return NULL;
	grown = allocate((existing + count) * sizeof(*grown));
	if (!grown)
		return NULL;
	memcpy(grown, instances, existing * sizeof(*grown));
	for (index = 0; index < existing; index++)
	{
		long instance_salt = (unsigned long)instances[index].tag_index >> 16;

		if (instance_salt > salt)
			salt = instance_salt;
	}
	menu_tags.original_instances = instances;
	menu_tags.original_count = existing;
	*first_index = existing;
	*first_salt = salt + 1;
	return grown;
}

static long next_tag(void)
{
	long tag_index = ((build.first_salt + build.next) << 16) | (build.first_index + build.next);

	build.next++;
	return tag_index;
}

static void instance_set(struct cache_file_tag_instance *instances, long group_tag, long tag_index, char const *name,
	char const *suffix, void *definition)
{
	struct cache_file_tag_instance *instance = &instances[DATUM_INDEX_TO_ABSOLUTE_INDEX(tag_index)];
	char *copy = allocate((long)strlen(PC_MENU_TAG_PREFIX) + (long)strlen(name) + (long)strlen(suffix) + 1);

	if (copy)
	{
		char *character;

		strcpy(copy, PC_MENU_TAG_PREFIX);
		strcat(copy, name);
		strcat(copy, suffix);
		/* (named as the map's tags are) */
		for (character = copy; *character; character++)
		{
			if (*character == '/')
				*character = '\\';
		}
	}
	instance->group_tag = group_tag;
	instance->parent_group_tags[0] = NONE;
	instance->parent_group_tags[1] = NONE;
	instance->tag_index = tag_index;
	instance->name = copy;
	instance->base_address = definition;
}

static void menu_tags_release(void)
{
	long index;

	for (index = 0; index < menu_tags.bitmap_count; index++)
	{
		if (menu_tags.bitmaps[index]->hardware_format)
			rasterizer_bitmap_delete(menu_tags.bitmaps[index]);
	}
	halo_menus_art_forget();
	if (menu_tags.original_instances)
		cache_files_set_tag_instances(menu_tags.original_instances, menu_tags.original_count);
	/* (the game's free takes no NULL: settings is, with no setting spinners) */
	for (index = 0; index < menu_tags.block_count; index++)
		free(menu_tags.blocks[index]);
	if (menu_tags.blocks)
		free(menu_tags.blocks);
	if (menu_tags.bitmaps)
		free(menu_tags.bitmaps);
	if (menu_tags.settings)
		free(menu_tags.settings);
	memset(&menu_tags, 0, sizeof(menu_tags));
}

/* ---------- public code */

void menu_tags_loaded(
	char const *map_name)
{
	struct halo_menus const *menus;
	struct cache_file_tag_instance *instances;
	long widget_count, own_lists = 0, total, index;

	if (strcmp(map_name, "ui") || strcmp(config_string("display.menus"), "pc"))
		return;
	menus = halo_menus_load();
	if (!menus)
		return;
	memset(&build, 0, sizeof(build));
	build.menus = menus;
	widget_count = menus->widget_count;
	build.widget_tags = malloc((widget_count + 1) * sizeof(long));
	build.text_tags = malloc((widget_count + 1) * sizeof(long));
	build.spinner_tags = malloc((widget_count + 1) * sizeof(long));
	build.bitmap_tags = malloc((menus->bitmap_count + 1) * sizeof(long));
	build.strings_tags = malloc((menus->string_list_count + 1) * sizeof(long));
	for (index = 0; index < widget_count; index++)
	{
		own_lists += (menus->widgets[index].text != NULL) + (menus->widgets[index].strings != NULL);
		/* (names are unique) */
		if (widget_named(menus->widgets[index].name) != index)
			problem(menus->widgets[index].file, menus->widgets[index].line, "two widgets are named",
				menus->widgets[index].name);
	}
	total = widget_count + own_lists + menus->string_list_count + menus->bitmap_count;
	instances = build.failed ? NULL : instances_grow(total, &build.first_index, &build.first_salt);
	if (!instances)
		goto failed;
	/* each new tag's index */
	for (index = 0; index < widget_count; index++)
	{
		build.widget_tags[index] = next_tag();
		build.text_tags[index] = menus->widgets[index].text ? next_tag() : NONE;
		build.spinner_tags[index] = menus->widgets[index].strings ? next_tag() : NONE;
	}
	for (index = 0; index < menus->string_list_count; index++)
		build.strings_tags[index] = next_tag();
	for (index = 0; index < menus->bitmap_count; index++)
		build.bitmap_tags[index] = next_tag();
	/* the tags: the bitmaps and strings first, which the widgets name; the
	widgets' names in the table before any is built, which they find there */
	for (index = 0; index < menus->bitmap_count && !build.failed; index++)
	{
		instance_set(instances, BITMAP_GROUP_TAG, build.bitmap_tags[index], menus->bitmaps[index].name, "",
			bitmap_build(&menus->bitmaps[index], build.bitmap_tags[index]));
	}
	for (index = 0; index < menus->string_list_count && !build.failed; index++)
	{
		struct halo_menu_strings const *list = &menus->string_lists[index];

		instance_set(instances, UNICODE_STRING_LIST_TAG, build.strings_tags[index], list->name, "",
			string_list_build(menus->strings + list->first, list->count));
	}
	for (index = 0; index < widget_count && !build.failed; index++)
	{
		struct halo_menu_widget const *widget = &menus->widgets[index];

		if (widget->text)
		{
			instance_set(instances, UNICODE_STRING_LIST_TAG, build.text_tags[index], widget->name, " text",
				string_list_build(&widget->text, 1));
		}
		if (widget->strings)
		{
			char const *pieces[MAXIMUM_STRINGS];
			long count = split(widget->strings, pieces);

			instance_set(instances, UNICODE_STRING_LIST_TAG, build.spinner_tags[index], widget->name, " strings",
				string_list_build(pieces, count));
		}
		instance_set(instances, UI_WIDGET_DEFINITION_TAG, build.widget_tags[index], widget->name, "", NULL);
	}
	if (build.failed)
		goto failed;
	cache_files_set_tag_instances(instances, build.first_index + total);
	for (index = 0; index < widget_count && !build.failed; index++)
		instances[DATUM_INDEX_TO_ABSOLUTE_INDEX(build.widget_tags[index])].base_address = widget_build(index);
	if (build.failed)
		goto failed;
	if (widget_named(menus->root) != NONE)
	{
		snprintf(menu_tags.root, sizeof(menu_tags.root), "%s%s", PC_MENU_TAG_PREFIX, menus->root);
		for (index = 0; menu_tags.root[index]; index++)
		{
			if (menu_tags.root[index] == '/')
				menu_tags.root[index] = '\\';
		}
	}
	else
	{
		platform_log("menus: there is no widget named %s, the main menu", menus->root);
	}
	menu_tags.loaded = TRUE;
	platform_log("menus: %ld widgets, %ld string lists and %ld bitmaps added to the map's %ld tags",
		widget_count, own_lists + menus->string_list_count, menus->bitmap_count, build.first_index);
	goto done;

failed:
	platform_log("menus: not added; using the game's own menus");
	menu_tags_release();

done:
	free(build.widget_tags);
	free(build.text_tags);
	free(build.spinner_tags);
	free(build.strings_tags);
	free(build.bitmap_tags);
	memset(&build, 0, sizeof(build));
}

void menu_tags_unloaded(
	void)
{
	if (menu_tags.loaded || menu_tags.block_count)
		menu_tags_release();
}

char const *pc_menus_root_name(
	void)
{
	/* (debug.menu_open: a screen to see, a profile being edited for the
	settings' screens) */
	char const *open = config_string("debug.menu_open");

	if (menu_tags.loaded && *open && build.menus == NULL)
	{
		static char name[300];
		long index;

		snprintf(name, sizeof(name), "%s%s", PC_MENU_TAG_PREFIX, open);
		for (index = 0; name[index]; index++)
		{
			if (name[index] == '/')
				name[index] = '\\';
		}
		if (tag_loaded(UI_WIDGET_DEFINITION_TAG, name) != NONE)
		{
			extern boolean pc_menu_profile_edit_begin(void);

			pc_menu_profile_edit_begin();
			return name;
		}
		platform_log("menus: debug.menu_open: there is no widget named %s", open);
	}
	return menu_tags.loaded && menu_tags.root[0] ? menu_tags.root : "ui\\shell\\main_menu\\main_menu";
}

boolean pc_menu_tag(
	long tag_index)
{
	return menu_tags.loaded && tag_index != NONE &&
		DATUM_INDEX_TO_ABSOLUTE_INDEX(tag_index) >= menu_tags.original_count;
}

char const *pc_menu_function_name(
	long function_index)
{
	return function_index >= 0 && function_index < (long)NUMBEROF(port_function_names) ?
		port_function_names[function_index] : NULL;
}

/* a widget definition's string_index (port settings help: its label's) */
short pc_menu_string_index(
	long definition_index)
{
	struct ui_widget_definition *definition = tag_get(UI_WIDGET_DEFINITION_TAG, definition_index);

	return definition ? definition->string_list_index : NONE;
}

/* the screen the engine opens by name after a network game (the Xbox's),
or ours where the PC version's menus are up: the host's map select, the
lobby */
char const *pc_menus_screen(
	char const *name)
{
	static struct
	{
		char const *game_name;
		char const *menu_name;
	} const screens[] =
	{
		{ "ui\\shell\\main_menu\\multiplayer_type_select\\connected\\connected_map_select_postgame_wrapper",
			"pc\\main_menu\\multiplayer_type_select\\connected\\connected_map_select_wrapper" },
		{ "ui\\shell\\main_menu\\multiplayer_type_select\\connected\\pregame\\connected_pregame_screen",
			"pc\\main_menu\\multiplayer_type_select\\lobby\\lobby_screen" },
	};
	short index;

	if (!menu_tags.loaded)
		return name;
	for (index = 0; index < NUMBEROF(screens); index++)
	{
		if (!strcmp(name, screens[index].game_name))
			return screens[index].menu_name;
	}
	return name;
}

char const *pc_menu_game_data_input_name(
	long function_index)
{
	return function_index >= 0 && function_index < (long)NUMBEROF(port_game_data_input_names) ?
		port_game_data_input_names[function_index] : NULL;
}

struct pc_menu_setting *pc_menu_setting_get(
	long definition_index)
{
	long index;

	for (index = 0; index < menu_tags.setting_count; index++)
	{
		if (menu_tags.settings[index].definition_index == definition_index)
			return &menu_tags.settings[index];
	}
	return NULL;
}
