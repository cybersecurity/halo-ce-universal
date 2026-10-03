/*
MENU_FILES.C

The menus written in XML (halo_menus.h): the files embedded from
port/assets/menus (menu_files.h), each replaced by a file of the same name
in a menus folder beside config.toml, and on the desktop any other .xml file
there too, read with Expat (port/third_party/expat) into the description the
game builds its widget tags from (port/linux/game/menu_tags.c).

Only the files' shape is checked here: their elements, which attributes
each may have, and numbers; the game checks what the names mean, against
the loaded map. Any problem is logged with its file and line, and then no
file is used: the game keeps its own menus.

Also the menus' art: the PNGs of their bitmaps' frames, each drawn for the
D3D texture the game made for it (menu_art_texture, xbox_textures.c), as the
high-res HUD's are (hud_hires.c).
*/

#include "halo_menus.h"
#include "hud_hires.h"
#include "menu_files.h"
#include "platform.h"
#include "port_config.h"
#include "xgpu.h"

#include "expat.h"

#include <SDL3/SDL.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXIMUM_DEPTH 32
#define MAXIMUM_ART 1024

/* ---------- the files */

struct menu_file
{
	char *path;
	const unsigned char *data;
	unsigned long size;
	int loaded;
};

static struct menu_file *files;
static long file_count;
static char folder[1024];

static long file_find(const char *path)
{
	long index;

	for (index = 0; index < file_count; index++)
	{
		if (!strcmp(files[index].path, path))
			return index;
	}
	return -1;
}

static void file_add(const char *path, const unsigned char *data, unsigned long size, int loaded)
{
	long index = file_find(path);

	if (index < 0)
	{
		files = realloc(files, (file_count + 1) * sizeof(*files));
		index = file_count++;
		files[index].path = strdup(path);
	}
	else if (files[index].loaded)
	{
		free((void *)files[index].data);
	}
	files[index].data = data;
	files[index].size = size;
	files[index].loaded = loaded;
}

static unsigned char *file_read(const char *path, unsigned long *size)
{
#ifdef HALO_ANDROID
	FILE *file = fopen(path, "rb");
	unsigned char *data = NULL;
	long length;

	if (!file)
		return NULL;
	if (fseek(file, 0, SEEK_END) == 0 && (length = ftell(file)) >= 0 && fseek(file, 0, SEEK_SET) == 0)
	{
		data = malloc((size_t)length + 1);
		if (data && fread(data, 1, (size_t)length, file) == (size_t)length)
		{
			*size = (unsigned long)length;
		}
		else
		{
			free(data);
			data = NULL;
		}
	}
	fclose(file);
	return data;
#else
	size_t length = 0;
	void *loaded = SDL_LoadFile(path, &length);
	unsigned char *data;

	if (!loaded)
		return NULL;
	data = malloc(length + 1);
	if (data)
		memcpy(data, loaded, length);
	SDL_free(loaded);
	*size = (unsigned long)length;
	return data;
#endif
}

/* the embedded files, then those of the menus folder: one of the same name
replaces an embedded one, and (on the desktop, where the folder can be
listed) another .xml file is added */
static void files_gather(void)
{
	unsigned int index;
	char path[1200];

	config_folder(folder, sizeof(folder));
	snprintf(folder + strlen(folder), sizeof(folder) - strlen(folder), "menus");
	for (index = 0; index < menu_files_embedded_count; index++)
	{
		const struct menu_file_embedded *embedded = &menu_files_embedded[index];
		unsigned long size = 0;
		unsigned char *data;

		snprintf(path, sizeof(path), "%s/%s", folder, embedded->path);
		data = file_read(path, &size);
		if (data)
		{
			platform_log("menus: %s replaces the built-in one", path);
			file_add(embedded->path, data, size, 1);
		}
		else
		{
			file_add(embedded->path, (const unsigned char *)embedded->data, embedded->size, 0);
		}
	}
#ifndef HALO_ANDROID
	{
		/* (the folder's own and its folders': SDL's * does not cross a /) */
		static const char *const patterns[] = { "*.xml", "*/*.xml" };
		int pattern;

		for (pattern = 0; pattern < 2; pattern++)
		{
		int count = 0;
		char **names = SDL_GlobDirectory(folder, patterns[pattern], 0, &count);
		int name;

		for (name = 0; names && name < count; name++)
		{
			unsigned long size = 0;
			unsigned char *data;

			if (file_find(names[name]) >= 0)
				continue;
			snprintf(path, sizeof(path), "%s/%s", folder, names[name]);
			data = file_read(path, &size);
			if (data)
			{
				platform_log("menus: adding %s", path);
				file_add(names[name], data, size, 1);
			}
		}
		SDL_free(names);
		}
	}
#endif
}

/* a file's data (a bitmap's PNG, by the path its <bitmap> gives): the
menus folder's, else the embedded one */
static const unsigned char *file_data(const char *path, unsigned long *size)
{
	long index = file_find(path);

	if (index < 0)
	{
		char full[1200];
		unsigned char *data;

		snprintf(full, sizeof(full), "%s/%s", folder, path);
		data = file_read(full, size);
		if (!data)
			return NULL;
		file_add(path, data, *size, 1);
		index = file_find(path);
	}
	*size = files[index].size;
	return files[index].data;
}

/* ---------- reading */

enum element
{
	_element_none,
	_element_menus,
	_element_bitmap,
	_element_frame,
	_element_strings,
	_element_string,
	_element_widget,
	_element_child,
	_element_on,
	_element_data,
	_element_conditional,
	_element_replace,
};

/* an attribute an element may have: its text (text) or number (number,
with given set when it is there) */
struct attribute
{
	const char *name;
	const char **text;
	long *number;
	long *given;
};

struct reader
{
	struct halo_menus menus;
	const char *file;
	XML_Parser parser;
	int failed;
	/* the open elements, and the widget, bitmap or strings each is in */
	enum element elements[MAXIMUM_DEPTH];
	long owners[MAXIMUM_DEPTH];
	int depth;
	/* how deep inside an element for the other platform we are (0: not) */
	int skipping;
	/* the open <bitmap> has frames= (its <frame>s cannot be added too) */
	int bitmap_frames_attribute;
	/* each widget's last child, handler, input, conditional and replace,
	for adding the next */
	long *last_child, *last_handler, *last_input, *last_conditional, *last_replace;
};

static void reader_error(struct reader *reader, const char *format, ...)
{
	char message[512];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);
	if (!reader->failed)
	{
		platform_log("menus: %s:%lu: %s", reader->file,
			(unsigned long)XML_GetCurrentLineNumber(reader->parser), message);
	}
	reader->failed = 1;
	XML_StopParser(reader->parser, XML_FALSE);
}

static char *copy_length(const char *text, size_t length)
{
	char *result = malloc(length + 1);

	if (result)
	{
		memcpy(result, text, length);
		result[length] = 0;
	}
	return result;
}

static char *copy(const char *text)
{
	return text ? copy_length(text, strlen(text)) : NULL;
}

static int parse_long(struct reader *reader, const char *name, const char *text, long *value)
{
	char *end;

	errno = 0;
	*value = strtol(text, &end, 10);
	if (!*text || *end)
	{
		reader_error(reader, "%s=\"%s\" is not a whole number", name, text);
		return 0;
	}
	/* (the tags keep them in shorts) */
	if (errno == ERANGE || *value < -32768 || *value > 32767)
	{
		reader_error(reader, "%s=\"%s\" is out of range (-32768 to 32767)", name, text);
		return 0;
	}
	return 1;
}

/* the element's attributes into the table's fields; one not in the table
(but platform, which for_this_platform reads) is an error */
static void read_attributes(struct reader *reader, const char *element, const XML_Char **attributes,
	const struct attribute *table, size_t count)
{
	int index;

	for (index = 0; attributes[index] && !reader->failed; index += 2)
	{
		const char *name = attributes[index], *value = attributes[index + 1];
		size_t field;

		if (!strcmp(name, "platform"))
			continue;
		for (field = 0; field < count; field++)
		{
			if (strcmp(name, table[field].name))
				continue;
			if (table[field].text)
				*table[field].text = copy(value);
			else if (parse_long(reader, name, value, table[field].number) && table[field].given)
				*table[field].given = 1;
			break;
		}
		if (field == count)
			reader_error(reader, "<%s> has no attribute %s", element, name);
	}
}

static long current_line(struct reader *reader)
{
	return (long)XML_GetCurrentLineNumber(reader->parser);
}

/* whether the element is for this platform (its platform attribute) */
static int for_this_platform(struct reader *reader, const XML_Char **attributes)
{
	int index;

	for (index = 0; attributes[index]; index += 2)
	{
		if (!strcmp(attributes[index], "platform"))
		{
			const char *platform = attributes[index + 1];

			if (strcmp(platform, "desktop") && strcmp(platform, "android"))
			{
				reader_error(reader, "platform=\"%s\" is not \"desktop\" or \"android\"", platform);
				return 1;
			}
#ifdef HALO_ANDROID
			return !strcmp(platform, "android");
#else
			return !strcmp(platform, "desktop");
#endif
		}
	}
	return 1;
}

/* room for one more element in an array of count */
static void *grow(void *array, long count, size_t size)
{
	return realloc(array, (size_t)(count + 1) * size);
}

static long *grow_last(long *array, long count)
{
	array = grow(array, count, sizeof(*array));
	array[count] = HALO_MENU_NONE;
	return array;
}

/* adds index to a widget's list (first, and each item's next) */
#define LIST_ADD(reader, owner, first, last, items, index) \
	do \
	{ \
		if ((reader)->last[owner] == HALO_MENU_NONE) \
			(reader)->menus.widgets[owner].first = (index); \
		else \
			(reader)->menus.items[(reader)->last[owner]].next = (index); \
		(reader)->last[owner] = (index); \
	} while (0)

static void read_bitmap(struct reader *reader, const XML_Char **attributes)
{
	struct halo_menus *menus = &reader->menus;
	struct halo_menu_bitmap bitmap;
	const char *frames = NULL;
	long width = 0, height = 0;
	struct attribute table[] =
	{
		{ "name", &bitmap.name }, { "frames", &frames }, { "width", NULL, &width }, { "height", NULL, &height },
	};

	memset(&bitmap, 0, sizeof(bitmap));
	bitmap.first_frame = menus->frame_count;
	bitmap.file = reader->file;
	bitmap.line = current_line(reader);
	read_attributes(reader, "bitmap", attributes, table, sizeof(table) / sizeof(*table));
	reader->bitmap_frames_attribute = frames != NULL;
	/* (frames="a.png b.png", all width by height, or <frame>s) */
	while (frames && *frames && !reader->failed)
	{
		size_t length;

		frames += strspn(frames, " \t\r\n");
		length = strcspn(frames, " \t\r\n");
		if (!length)
			break;
		menus->frames = grow(menus->frames, menus->frame_count, sizeof(*menus->frames));
		memset(&menus->frames[menus->frame_count], 0, sizeof(*menus->frames));
		menus->frames[menus->frame_count].png = copy_length(frames, length);
		menus->frames[menus->frame_count].width = width;
		menus->frames[menus->frame_count].height = height;
		menus->frame_count++;
		bitmap.frame_count++;
		frames += length;
	}
	if (!reader->failed && !bitmap.name)
		reader_error(reader, "a <bitmap> needs a name");
	menus->bitmaps = grow(menus->bitmaps, menus->bitmap_count, sizeof(*menus->bitmaps));
	menus->bitmaps[menus->bitmap_count++] = bitmap;
}

static void read_frame(struct reader *reader, const XML_Char **attributes, long owner)
{
	struct halo_menus *menus = &reader->menus;
	struct halo_menu_frame frame;
	struct attribute table[] =
	{
		{ "png", &frame.png }, { "map", &frame.map }, { "index", NULL, &frame.index },
		{ "width", NULL, &frame.width }, { "height", NULL, &frame.height },
	};

	memset(&frame, 0, sizeof(frame));
	frame.index = -1;
	read_attributes(reader, "frame", attributes, table, sizeof(table) / sizeof(*table));
	if (!reader->failed && (frame.png != NULL) == (frame.map != NULL))
		reader_error(reader, "a <frame> has a png or a map bitmap, not both");
	else if (!reader->failed && frame.png && (frame.width <= 0 || frame.height <= 0))
		reader_error(reader, "a <frame> with a png needs width and height");
	else if (!reader->failed && frame.map && frame.index < 0)
		reader_error(reader, "a <frame> of a map bitmap needs its index");
	if (!reader->failed && reader->bitmap_frames_attribute)
		reader_error(reader, "a bitmap's <frame>s and frames= cannot be mixed");
	if (frame.index < 0)
		frame.index = 0;
	menus->frames = grow(menus->frames, menus->frame_count, sizeof(*menus->frames));
	menus->frames[menus->frame_count++] = frame;
	menus->bitmaps[owner].frame_count++;
}

static void read_strings(struct reader *reader, const XML_Char **attributes)
{
	struct halo_menus *menus = &reader->menus;
	struct halo_menu_strings strings;
	struct attribute table[] = { { "name", &strings.name } };

	memset(&strings, 0, sizeof(strings));
	strings.first = menus->string_count;
	strings.file = reader->file;
	strings.line = current_line(reader);
	read_attributes(reader, "strings", attributes, table, sizeof(table) / sizeof(*table));
	if (!reader->failed && !strings.name)
		reader_error(reader, "a <strings> needs a name");
	menus->string_lists = grow(menus->string_lists, menus->string_list_count, sizeof(*menus->string_lists));
	menus->string_lists[menus->string_list_count++] = strings;
}

static void read_string(struct reader *reader, const XML_Char **attributes, long owner)
{
	struct halo_menus *menus = &reader->menus;
	const char *text = NULL;
	struct attribute table[] = { { "text", &text } };

	read_attributes(reader, "string", attributes, table, sizeof(table) / sizeof(*table));
	if (!reader->failed && !text)
		reader_error(reader, "a <string> needs text");
	menus->strings = grow(menus->strings, menus->string_count, sizeof(*menus->strings));
	menus->strings[menus->string_count++] = text ? text : "";
	menus->string_lists[owner].count++;
}

static void child_add(struct reader *reader, long parent, long nested, const char *widget, long x, long y,
	const char *controller)
{
	struct halo_menus *menus = &reader->menus;
	struct halo_menu_child child;
	long index = menus->child_count;

	memset(&child, 0, sizeof(child));
	child.nested = nested;
	child.widget = widget;
	child.x = x;
	child.y = y;
	child.controller = controller;
	child.next = HALO_MENU_NONE;
	child.file = reader->file;
	child.line = current_line(reader);
	menus->children = grow(menus->children, menus->child_count, sizeof(*menus->children));
	menus->children[menus->child_count++] = child;
	LIST_ADD(reader, parent, first_child, last_child, children, index);
}

static void read_widget(struct reader *reader, const XML_Char **attributes, long parent)
{
	struct halo_menus *menus = &reader->menus;
	struct halo_menu_widget widget;
	long index = menus->widget_count;
	const char *child_controller = NULL;
	struct attribute table[] =
	{
		{ "name", &widget.name }, { "type", &widget.type }, { "controller", &widget.controller },
		{ "flags", &widget.flags }, { "bitmap", &widget.bitmap }, { "text", &widget.text },
		{ "strings", &widget.strings }, { "values", &widget.values }, { "setting", &widget.setting },
		{ "font", &widget.font }, { "color", &widget.color }, { "align", &widget.align },
		{ "description", &widget.description }, { "string_list", &widget.string_list },
		{ "list_flags", &widget.list_flags }, { "text_flags", &widget.text_flags },
		{ "header_bitmap", &widget.header_bitmap }, { "footer_bitmap", &widget.footer_bitmap },
		{ "header_bounds", &widget.header_bounds }, { "footer_bounds", &widget.footer_bounds },
		{ "child_controller", &child_controller },
		{ "x", NULL, &widget.x }, { "y", NULL, &widget.y }, { "left", NULL, &widget.left },
		{ "top", NULL, &widget.top }, { "width", NULL, &widget.width, &widget.has_width },
		{ "height", NULL, &widget.height, &widget.has_height }, { "text_x", NULL, &widget.text_x },
		{ "text_y", NULL, &widget.text_y }, { "auto_close", NULL, &widget.auto_close },
		{ "auto_close_fade", NULL, &widget.auto_close_fade }, { "string_index", NULL, &widget.string_index },
	};

	memset(&widget, 0, sizeof(widget));
	widget.parent = parent;
	widget.first_child = widget.first_handler = widget.first_input = HALO_MENU_NONE;
	widget.first_conditional = widget.first_replace = HALO_MENU_NONE;
	widget.file = reader->file;
	widget.line = current_line(reader);
	read_attributes(reader, "widget", attributes, table, sizeof(table) / sizeof(*table));
	if (!reader->failed && !widget.name)
		reader_error(reader, "a <widget> needs a name");
	menus->widgets = grow(menus->widgets, index, sizeof(*menus->widgets));
	menus->widgets[index] = widget;
	menus->widget_count++;
	reader->last_child = grow_last(reader->last_child, index);
	reader->last_handler = grow_last(reader->last_handler, index);
	reader->last_input = grow_last(reader->last_input, index);
	reader->last_conditional = grow_last(reader->last_conditional, index);
	reader->last_replace = grow_last(reader->last_replace, index);
	if (parent != HALO_MENU_NONE)
		child_add(reader, parent, index, NULL, widget.x, widget.y, child_controller);
}

static void read_child(struct reader *reader, const XML_Char **attributes, long owner)
{
	const char *widget = NULL, *controller = NULL;
	long x = 0, y = 0;
	struct attribute table[] =
	{
		{ "widget", &widget }, { "controller", &controller }, { "x", NULL, &x }, { "y", NULL, &y },
	};

	read_attributes(reader, "child", attributes, table, sizeof(table) / sizeof(*table));
	if (!reader->failed && !widget)
		reader_error(reader, "a <child> needs a widget");
	child_add(reader, owner, HALO_MENU_NONE, widget, x, y, controller);
}

static void read_handler(struct reader *reader, const XML_Char **attributes, long owner)
{
	struct halo_menus *menus = &reader->menus;
	struct halo_menu_handler handler;
	long index = menus->handler_count;
	const char *back = NULL, *branch = NULL;
	struct attribute table[] =
	{
		{ "event", &handler.event }, { "run", &handler.run }, { "script", &handler.script },
		{ "open", &handler.open }, { "replace", &handler.replace }, { "close", &handler.close },
		{ "widget", &handler.widget }, { "focus", &handler.focus }, { "reload", &handler.reload },
		{ "sound", &handler.sound }, { "otherwise", &handler.otherwise }, { "label", &handler.label },
		{ "back", &back }, { "branch", &branch },
	};

	memset(&handler, 0, sizeof(handler));
	handler.next = HALO_MENU_NONE;
	handler.file = reader->file;
	handler.line = current_line(reader);
	read_attributes(reader, "on", attributes, table, sizeof(table) / sizeof(*table));
	if ((back && strcmp(back, "true") && strcmp(back, "false")) ||
		(branch && strcmp(branch, "true") && strcmp(branch, "false")))
		reader_error(reader, "back and branch are \"true\" or \"false\"");
	handler.back = back && !strcmp(back, "true");
	handler.branch = branch && !strcmp(branch, "true");
	if (!reader->failed && !handler.event)
		reader_error(reader, "an <on> needs an event");
	menus->handlers = grow(menus->handlers, index, sizeof(*menus->handlers));
	menus->handlers[index] = handler;
	menus->handler_count++;
	LIST_ADD(reader, owner, first_handler, last_handler, handlers, index);
}

static void read_input(struct reader *reader, const XML_Char **attributes, long owner)
{
	struct halo_menus *menus = &reader->menus;
	struct halo_menu_input input;
	long index = menus->input_count;
	struct attribute table[] = { { "input", &input.input } };

	memset(&input, 0, sizeof(input));
	input.next = HALO_MENU_NONE;
	input.file = reader->file;
	input.line = current_line(reader);
	read_attributes(reader, "data", attributes, table, sizeof(table) / sizeof(*table));
	if (!reader->failed && !input.input)
		reader_error(reader, "a <data> needs an input");
	menus->inputs = grow(menus->inputs, index, sizeof(*menus->inputs));
	menus->inputs[index] = input;
	menus->input_count++;
	LIST_ADD(reader, owner, first_input, last_input, inputs, index);
}

static void read_conditional(struct reader *reader, const XML_Char **attributes, long owner)
{
	struct halo_menus *menus = &reader->menus;
	struct halo_menu_conditional conditional;
	long index = menus->conditional_count;
	const char *if_failed = NULL;
	struct attribute table[] = { { "widget", &conditional.widget }, { "if_failed", &if_failed } };

	memset(&conditional, 0, sizeof(conditional));
	conditional.next = HALO_MENU_NONE;
	conditional.file = reader->file;
	conditional.line = current_line(reader);
	read_attributes(reader, "conditional", attributes, table, sizeof(table) / sizeof(*table));
	if (if_failed && strcmp(if_failed, "true") && strcmp(if_failed, "false"))
		reader_error(reader, "if_failed is \"true\" or \"false\"");
	conditional.if_failed = if_failed && !strcmp(if_failed, "true");
	if (!reader->failed && !conditional.widget)
		reader_error(reader, "a <conditional> needs a widget");
	menus->conditionals = grow(menus->conditionals, index, sizeof(*menus->conditionals));
	menus->conditionals[index] = conditional;
	menus->conditional_count++;
	LIST_ADD(reader, owner, first_conditional, last_conditional, conditionals, index);
}

static void read_replace(struct reader *reader, const XML_Char **attributes, long owner)
{
	struct halo_menus *menus = &reader->menus;
	struct halo_menu_replace replace;
	long index = menus->replace_count;
	struct attribute table[] = { { "search", &replace.search }, { "function", &replace.function } };

	memset(&replace, 0, sizeof(replace));
	replace.next = HALO_MENU_NONE;
	replace.file = reader->file;
	replace.line = current_line(reader);
	read_attributes(reader, "replace", attributes, table, sizeof(table) / sizeof(*table));
	if (!reader->failed && !replace.search)
		reader_error(reader, "a <replace> needs a search");
	menus->replaces = grow(menus->replaces, index, sizeof(*menus->replaces));
	menus->replaces[index] = replace;
	menus->replace_count++;
	LIST_ADD(reader, owner, first_replace, last_replace, replaces, index);
}

static void read_menus(struct reader *reader, const XML_Char **attributes)
{
	const char *root = NULL;
	struct attribute table[] = { { "root", &root } };

	read_attributes(reader, "menus", attributes, table, sizeof(table) / sizeof(*table));
	if (root)
		reader->menus.root = root;
}

static void XMLCALL element_start(void *data, const XML_Char *name, const XML_Char **attributes)
{
	struct reader *reader = data;
	enum element parent = reader->depth ? reader->elements[reader->depth - 1] : _element_none;
	long owner = reader->depth ? reader->owners[reader->depth - 1] : HALO_MENU_NONE;
	enum element element = _element_none;

	if (reader->failed)
		return;
	if (reader->skipping || !for_this_platform(reader, attributes))
	{
		reader->skipping++;
		return;
	}
	if (reader->depth == MAXIMUM_DEPTH)
	{
		reader_error(reader, "elements nested too deeply");
		return;
	}
	if (!strcmp(name, "menus") && parent == _element_none)
	{
		element = _element_menus;
		read_menus(reader, attributes);
	}
	else if (!strcmp(name, "bitmap") && parent == _element_menus)
	{
		element = _element_bitmap;
		read_bitmap(reader, attributes);
		owner = reader->menus.bitmap_count - 1;
	}
	else if (!strcmp(name, "frame") && parent == _element_bitmap)
	{
		element = _element_frame;
		read_frame(reader, attributes, owner);
	}
	else if (!strcmp(name, "strings") && parent == _element_menus)
	{
		element = _element_strings;
		read_strings(reader, attributes);
		owner = reader->menus.string_list_count - 1;
	}
	else if (!strcmp(name, "string") && parent == _element_strings)
	{
		element = _element_string;
		read_string(reader, attributes, owner);
	}
	else if (!strcmp(name, "widget") && (parent == _element_menus || parent == _element_widget))
	{
		element = _element_widget;
		read_widget(reader, attributes, parent == _element_widget ? owner : HALO_MENU_NONE);
		owner = reader->menus.widget_count - 1;
	}
	else if (parent == _element_widget)
	{
		if (!strcmp(name, "child"))
		{
			element = _element_child;
			read_child(reader, attributes, owner);
		}
		else if (!strcmp(name, "on"))
		{
			element = _element_on;
			read_handler(reader, attributes, owner);
		}
		else if (!strcmp(name, "data"))
		{
			element = _element_data;
			read_input(reader, attributes, owner);
		}
		else if (!strcmp(name, "conditional"))
		{
			element = _element_conditional;
			read_conditional(reader, attributes, owner);
		}
		else if (!strcmp(name, "replace"))
		{
			element = _element_replace;
			read_replace(reader, attributes, owner);
		}
	}
	if (element == _element_none)
	{
		reader_error(reader, "<%s> cannot be here", name);
		return;
	}
	reader->elements[reader->depth] = element;
	reader->owners[reader->depth] = owner;
	reader->depth++;
}

static void XMLCALL element_end(void *data, const XML_Char *name)
{
	struct reader *reader = data;

	(void)name;
	if (reader->skipping)
		reader->skipping--;
	else if (reader->depth)
		reader->depth--;
}

static void XMLCALL text(void *data, const XML_Char *characters, int length)
{
	struct reader *reader = data;
	int index;

	if (reader->failed || reader->skipping)
		return;
	for (index = 0; index < length; index++)
	{
		if (!strchr(" \t\r\n", characters[index]))
		{
			reader_error(reader, "text belongs in an attribute (text=\"...\")");
			return;
		}
	}
}

static int read_file(struct reader *reader, const struct menu_file *file)
{
	reader->file = file->path;
	reader->depth = 0;
	reader->skipping = 0;
	reader->parser = XML_ParserCreate("UTF-8");
	if (!reader->parser)
	{
		platform_log("menus: %s: no XML parser (out of memory)", file->path);
		reader->failed = 1;
		return 0;
	}
	XML_SetUserData(reader->parser, reader);
	XML_SetElementHandler(reader->parser, element_start, element_end);
	XML_SetCharacterDataHandler(reader->parser, text);
	if (XML_Parse(reader->parser, (const char *)file->data, (int)file->size, XML_TRUE) == XML_STATUS_ERROR &&
		!reader->failed)
	{
		platform_log("menus: %s:%lu:%lu: %s", file->path, (unsigned long)XML_GetCurrentLineNumber(reader->parser),
			(unsigned long)XML_GetCurrentColumnNumber(reader->parser),
			XML_ErrorString(XML_GetErrorCode(reader->parser)));
		reader->failed = 1;
	}
	XML_ParserFree(reader->parser);
	return !reader->failed;
}

/* ---------- public code */

struct halo_menus const *halo_menus_load(void)
{
	static int read;
	static struct halo_menus menus;
	static int succeeded;
	struct reader reader;
	long index;

	if (read)
		return succeeded ? &menus : NULL;
	read = 1;
	files_gather();
	memset(&reader, 0, sizeof(reader));
	for (index = 0; index < file_count && !reader.failed; index++)
	{
		size_t length = strlen(files[index].path);

		if (length > 4 && !strcmp(files[index].path + length - 4, ".xml"))
			read_file(&reader, &files[index]);
	}
	free(reader.last_child);
	free(reader.last_handler);
	free(reader.last_input);
	free(reader.last_conditional);
	free(reader.last_replace);
	if (reader.failed || !reader.menus.widget_count)
	{
		if (reader.failed)
			platform_log("menus: the files have a problem; using the game's own menus");
		return NULL;
	}
	if (!reader.menus.root)
		reader.menus.root = "main_menu";
	menus = reader.menus;
	succeeded = 1;
	return &menus;
}

long halo_menus_utf16(char const *utf8, unsigned short *out, long capacity)
{
	const unsigned char *in = (const unsigned char *)utf8;
	long count = 0;

	while (*in && count < capacity - 1)
	{
		unsigned long code = *in++;

		if (code == '\\' && *in == 'n')
		{
			in++;
			if (count + 2 >= capacity)
				break;
			out[count++] = '\r';
			out[count++] = '\n';
			continue;
		}
		if (code >= 0xC0)
		{
			int more = code >= 0xF0 ? 3 : code >= 0xE0 ? 2 : 1;

			code &= 0x3F >> more;
			while (more-- && (*in & 0xC0) == 0x80)
				code = (code << 6) | (*in++ & 0x3F);
		}
		/* (beyond the 16-bit characters: the game's fonts have none) */
		out[count++] = code < 0x10000 ? (unsigned short)code : '?';
	}
	out[count++] = 0;
	return count;
}

void halo_menus_log(char const *file, long line, char const *message, char const *detail)
{
	platform_log("menus: %s:%ld: %s%s%s", file ? file : "?", line, message, detail ? " " : "", detail ? detail : "");
}

/* ---------- art */

static struct
{
	unsigned long data;
	char *png;
	unsigned int texture;
	unsigned long levels;
	int failed;
} art[MAXIMUM_ART];
static long art_count;

void halo_menus_art_register(void const *texture, char const *png)
{
	unsigned long data = ((const unsigned long *)texture)[1];
	long index;

	for (index = 0; index < art_count; index++)
	{
		if (art[index].data == data)
			break;
	}
	if (index == art_count)
	{
		if (art_count == MAXIMUM_ART)
		{
			platform_log("menus: more than %d bitmaps; %s is not drawn", MAXIMUM_ART, png);
			return;
		}
		art_count++;
	}
	else if (art[index].png && !strcmp(art[index].png, png))
	{
		return;
	}
	else
	{
		free(art[index].png);
		if (art[index].texture)
			glDeleteTextures(1, &art[index].texture);
	}
	memset(&art[index], 0, sizeof(art[index]));
	art[index].data = data;
	art[index].png = strdup(png);
}

void halo_menus_art_forget(void)
{
	long index;

	for (index = 0; index < art_count; index++)
	{
		free(art[index].png);
		if (art[index].texture)
			glDeleteTextures(1, &art[index].texture);
	}
	art_count = 0;
}

unsigned int menu_art_texture(unsigned long data, unsigned long *levels)
{
	long index;

	for (index = 0; index < art_count; index++)
	{
		if (art[index].data == data)
			break;
	}
	if (index == art_count || art[index].failed)
		return 0;
	if (!art[index].texture)
	{
		unsigned long size = 0;
		const unsigned char *png = file_data(art[index].png, &size);

		art[index].texture = png ? hud_hires_png_texture(png, size, &art[index].levels) : 0;
		if (!art[index].texture)
		{
			platform_log("menus: could not draw %s", art[index].png);
			art[index].failed = 1;
			return 0;
		}
	}
	*levels = art[index].levels;
	return art[index].texture;
}
