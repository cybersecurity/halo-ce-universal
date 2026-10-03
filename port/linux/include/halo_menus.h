/*
HALO_MENUS.H

The menus written in XML (port/assets/menus, and a menus folder beside
config.toml), as the platform layer reads them (port/linux/src/menu_files.c)
for the game, which builds them into widget tags when ui.map loads
(port/linux/game/menu_tags.c). Plain C89 types, for both: each element keeps
its attributes as written, and the file and line it came from; the game reads
their meaning.

A file holds <menus> with <bitmap> (holding <frame>s), <strings> (holding
<string>s) and <widget> elements; a <widget> holds <widget> and <child>
(its children), <on> (an event handler), <data> (a game data input),
<conditional> and <replace> elements. port/assets/menus/README.md describes
them.
*/

#ifndef HALO_MENUS_H
#define HALO_MENUS_H

#define HALO_MENU_NONE (-1L)
/* the port's event handler and game data functions (menu_functions.c) are
numbered from here, clear of the Xbox's (0 to 101 and 0 to 40) and the PC's
(to 189 and 58) */
#define PC_MENU_FUNCTION_BASE 256

/* a frame: a PNG drawn at width by height, or the map's bitmap's frame
(map: its bitmap group tag, index: the bitmap's number in it) */
struct halo_menu_frame
{
	char const *png;
	char const *map;
	long index;
	long width, height;
};

struct halo_menu_bitmap
{
	char const *name;
	/* its frames: frame_count from first_frame in the frames */
	long first_frame, frame_count;
	char const *file;
	long line;
};

struct halo_menu_strings
{
	char const *name;
	/* its strings: count from first in the strings */
	long first, count;
	char const *file;
	long line;
};

struct halo_menu_handler
{
	char const *event, *run, *script, *open, *replace, *close, *widget, *focus, *reload, *sound, *otherwise, *label;
	long back, branch;
	long next;
	char const *file;
	long line;
};

struct halo_menu_input
{
	char const *input;
	long next;
	char const *file;
	long line;
};

/* a child: a <widget> inside its parent (nested), or one named (<child>) */
struct halo_menu_child
{
	long nested;
	char const *widget;
	long x, y;
	char const *controller;
	long next;
	char const *file;
	long line;
};

struct halo_menu_conditional
{
	char const *widget;
	long if_failed;
	long next;
	char const *file;
	long line;
};

struct halo_menu_replace
{
	char const *search, *function;
	long next;
	char const *file;
	long line;
};

struct halo_menu_widget
{
	char const *name, *type, *controller, *flags, *bitmap, *text, *strings, *values, *setting, *font, *color,
		*align, *description, *string_list, *list_flags, *text_flags, *header_bitmap, *footer_bitmap,
		*header_bounds, *footer_bounds;
	long x, y, left, top, width, height, text_x, text_y, auto_close, auto_close_fade, string_index;
	long has_width, has_height;
	/* indices in their arrays, or HALO_MENU_NONE */
	long parent, first_child, first_handler, first_input, first_conditional, first_replace;
	char const *file;
	long line;
};

struct halo_menus
{
	struct halo_menu_bitmap *bitmaps;
	long bitmap_count;
	struct halo_menu_frame *frames;
	long frame_count;
	struct halo_menu_strings *string_lists;
	long string_list_count;
	char const **strings;
	long string_count;
	struct halo_menu_widget *widgets;
	long widget_count;
	struct halo_menu_child *children;
	long child_count;
	struct halo_menu_handler *handlers;
	long handler_count;
	struct halo_menu_input *inputs;
	long input_count;
	struct halo_menu_conditional *conditionals;
	long conditional_count;
	struct halo_menu_replace *replaces;
	long replace_count;
	/* the main menu: <menus root="..."> (the last given), else "main_menu" */
	char const *root;
};

/* the menus, read from the files (once; the same each call after); NULL if
there are none, or a file has an error (logged) */
struct halo_menus const *halo_menus_load(void);
/* text as UTF-16, its terminator included, "\n" written as the game's line
breaks ("\r\n"): the number of characters written (at most capacity) */
long halo_menus_utf16(char const *utf8, unsigned short *out, long capacity);
/* logs a problem in a file's element */
void halo_menus_log(char const *file, long line, char const *message, char const *detail);

/* menu art: the D3D texture (as the game's bitmap holds it) a frame's PNG is
drawn for, until halo_menus_art_forget */
void halo_menus_art_register(void const *texture, char const *png);
void halo_menus_art_forget(void);

#endif
