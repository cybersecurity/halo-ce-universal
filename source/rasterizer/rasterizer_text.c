/*
RASTERIZER_TEXT.C

symbols in this file:
00172E80 0010:
	_code_00172e80 (0000)
00172E90 0010:
	_code_00172e90 (0000)
00172EA0 0090:
	_rasterizer_text_cache_initialize (0000)
00172F30 0010:
	_rasterizer_text_set_shadow_color (0000)
00172F40 0030:
	_rasterizer_text_cache_flush (0000)
00172F70 0030:
	_rasterizer_text_cache_dispose (0000)
00172FA0 0020:
	_code_00172fa0 (0000)
00172FC0 00b0:
	_code_00172fc0 (0000)
00173070 0060:
	_code_00173070 (0000)
001730D0 0380:
	_code_001730d0 (0000)
00173450 00f0:
	_code_00173450 (0000)
00173540 0170:
	_code_00173540 (0000)
001736B0 0200:
	_rasterizer_draw_string (0000)
001738B0 0200:
	_rasterizer_draw_unicode_string (0000)
0029EEE0 0033:
	??_C@_0DD@DKOHMJNA@?$CD?$CD?$CD?5ERROR?5failed?5to?5initialize?5h@ (0000)
0029EF14 0026:
	??_C@_0CG@HPKDNNGC@?$CBhardware_character_cache?4initia@ (0000)
0029EF3C 002c:
	??_C@_0CM@KJINBGGM@c?3?2halo?2SOURCE?2rasterizer?2raster@ (0000)
0029EF68 0009:
	??_C@_08KDNNBGOA@x0?5?$CG?$CG?5y0?$AA@ (0000)
0029EF78 0054:
	??_C@_0FE@BCPFIEIE@hardware_character_index?$DO?$DN0?5?$CG?$CG?5h@ (0000)
0029EFCC 0025:
	??_C@_0CF@POBHCEMM@hardware_character_cache?4initial@ (0000)
0029EFF4 0026:
	??_C@_0CG@JHCOKPHL@font?5cache?5overwrote?5character?5i@ (0000)
0029F01C 0013:
	??_C@_0BD@PIEBJAO@hardware_character?$AA@ (0000)
0029F030 0046:
	??_C@_0EG@CBPAFGHN@font_character?9?$DObitmap_height?$DM?$DNH@ (0000)
0029F078 0044:
	??_C@_0EE@KGLMBKON@font_character?9?$DObitmap_width?$DM?$DNHA@ (0000)
0029F0C0 0068:
	??_C@_0GI@IDEJCHPO@font_character?$DN?$DNhardware_charact@ (0000)
0029F128 0074:
	??_C@_0HE@KFKECHAF@font_character?9?$DOhardware_charact@ (0000)
0030D4D0 0002:
	_data_0030d4d0 (0000)
004B82C0 0816:
	_bss_004b82c0 (0000)
*/


/* ---------- headers */

#include "cseries/cseries.h"
#include "cseries/errors.h"
#include "bitmaps/bitmap_group.h"
#include "bitmaps/bitmaps.h"
#include "math/integer_math.h"
#include "rasterizer/rasterizer.h"
#include "rasterizer/rasterizer_console_vars.h"
#include "rasterizer/rasterizer_text.h"
#include "rasterizer/xbox/rasterizer_xbox_hardware_bitmaps.h"
#include "render/render.h"
#include "text/draw_string.h"
#include "text/font_group.h"
#include "text/unicode.h"
#include "tag_files/tag_files.h"

/* port: the high-res text (port/linux/src/text_hires.c): the text drawn with
fonts at the display's resolution, laid out as before. Characters are drawn
from an atlas of the fonts' glyphs that a placeholder bitmap stands for. */
struct text_hires_glyph
{
	float left, top, right, bottom;
	float u0, v0, u1, v1;
	float advance;
};

long text_hires_font(char const *tag_name, float cap_height, float oversample);
int text_hires_covers(long font, unsigned long code);
int text_hires_glyph(long font, unsigned long code, struct text_hires_glyph *glyph);
void text_hires_register_atlas(unsigned long const *texture, unsigned long width, unsigned long height);

/* ---------- constants */

enum
{
	HARDWARE_CHARACTER_CACHE_BITMAP_WIDTH = 128,
	HARDWARE_CHARACTER_CACHE_BITMAP_HEIGHT = 128,
	MAXIMUM_HARDWARE_CHARACTERS = 256,
	/* port: the high-res text atlas's placeholder (its texels are the units
	of the atlas's glyphs), and the fonts remembered */
	HIRES_TEXT_ATLAS_BITMAP_SIZE = 256,
	MAXIMUM_HIRES_TEXT_FONTS = 8,
};

enum
{
	_bitmap_format_a4r4g4b4 = 9,
};

enum
{
	_rasterizer_target_render_primary = 0,
	_shader_framebuffer_blend_function_alpha_blend = 0,
};

/* ---------- macros */

/* ---------- structures */

struct font_character
{
	word character;
	short character_width;
	short bitmap_width;
	short bitmap_height;
	short bitmap_origin_x;
	short bitmap_origin_y;
	short hardware_character_index;
	short pad;
	long pixels_offset;
};

struct parse_string_state;

typedef void (*draw_character_proc)(
	struct parse_string_state *state,
	struct font_header *font,
	struct font_character *font_character,
	unsigned long color,
	short x0,
	short y0,
	short x,
	short y,
	short dx,
	short dy);

struct hardware_character
{
	struct font_character *character;
	short x0;
	short y0;
};

struct hardware_character_cache
{
	boolean initialized;
	byte unused1;
	short read_index;
	short write_index;
	short x0;
	short y0;
	short maximum_character_height;
	struct bitmap_data *bitmap;
	struct hardware_character characters[MAXIMUM_HARDWARE_CHARACTERS];
};

/* ---------- prototypes */

static struct bitmap_data *hardware_character_cache_get_bitmap(
	void);
static void hardware_character_cache_get_origin(
	short hardware_character_index,
	short *x0,
	short *y0);
static short hardware_character_padding(
	struct font_character const *font_character);
static void flush_hardware_character(
	struct hardware_character *hardware_character);
static void cache_hardware_format_character(
	struct font_header *font,
	struct font_character *font_character);
static void rasterizer_draw_character(
	struct parse_string_state *state,
	struct font_header *font,
	struct font_character *font_character,
	unsigned long color,
	short x0,
	short y0,
	short x,
	short y,
	short dx,
	short dy);
static void rasterizer_draw_character_with_dropshadow(
	struct parse_string_state *state,
	struct font_header *font,
	struct font_character *font_character,
	unsigned long color,
	short x0,
	short y0,
	short x,
	short y,
	short dx,
	short dy);
static long hires_text_font_get(
	long font_index,
	real oversample);
static void rasterizer_text_draw_scaled_character(
	struct dynamic_screen_vertex const *vertices);
static void rasterizer_draw_hires_character(
	struct parse_string_state *state,
	struct font_header *font,
	struct font_character *font_character,
	unsigned long color,
	short x0,
	short y0,
	short x,
	short y,
	short dx,
	short dy);
static void rasterizer_draw_hires_character_with_dropshadow(
	struct parse_string_state *state,
	struct font_header *font,
	struct font_character *font_character,
	unsigned long color,
	short x0,
	short y0,
	short x,
	short y,
	short dx,
	short dy);

/* ---------- globals */


static struct hardware_character_cache hardware_character_cache;
static struct bitmap_data *hires_text_atlas = NULL;
static long hires_text_font = NONE;
/* port: the text's scale about a point (rasterizer_text_set_scale) */
static real text_scale = 1.0f;
static real text_scale_origin_x = 0.0f;
static real text_scale_origin_y = 0.0f;
static pixel32 global_shadow_color = 0;
static short rasterizer_text_unused = 0;
static short magic_number= 12;

/* ---------- public code */

void lock_rasterizer_text_data(
	void)
{
	return;
}

void unlock_rasterizer_text_data(
	void)
{
	return;
}

boolean
rasterizer_text_cache_initialize(
	void)
{
	struct bitmap_data *bitmap;
	boolean success = TRUE;

	match_assert("c:\\halo\\SOURCE\\rasterizer\\rasterizer_text.c", 118, !hardware_character_cache.initialized);

	bitmap = bitmap_2d_new(
		HARDWARE_CHARACTER_CACHE_BITMAP_WIDTH,
		HARDWARE_CHARACTER_CACHE_BITMAP_HEIGHT,
		0,
		_bitmap_format_a4r4g4b4);

	if (bitmap)
	{
		memset(&hardware_character_cache, 0, sizeof(hardware_character_cache));

		if (rasterizer_bitmap_new(bitmap))
		{
			hardware_character_cache.bitmap = bitmap;
			hardware_character_cache.initialized = TRUE;

			/* port: the high-res text atlas's placeholder */
			hires_text_atlas = bitmap_2d_new(
				HIRES_TEXT_ATLAS_BITMAP_SIZE,
				HIRES_TEXT_ATLAS_BITMAP_SIZE,
				0,
				_bitmap_format_a4r4g4b4);
			if (hires_text_atlas && !rasterizer_bitmap_new(hires_text_atlas))
			{
				bitmap_delete(hires_text_atlas);
				hires_text_atlas = NULL;
			}
			if (hires_text_atlas)
			{
				text_hires_register_atlas(
					(unsigned long const *)hires_text_atlas->hardware_format,
					HIRES_TEXT_ATLAS_BITMAP_SIZE,
					HIRES_TEXT_ATLAS_BITMAP_SIZE);
			}
		}
		else
		{
			error(_error_silent, "### ERROR failed to initialize hardware text cache");
			success = FALSE;
		}
	}
	else
	{
		error(_error_silent, "### ERROR failed to initialize hardware text cache");
		success = FALSE;
	}

	return success;
}

void
rasterizer_text_set_shadow_color(
	pixel32 shadow_color)
{
	global_shadow_color = shadow_color;

	return;
}

void
rasterizer_text_cache_flush(
	void)
{
	struct hardware_character *hardware_character;
	long hardware_character_count;

	if (hardware_character_cache.initialized)
	{
		hardware_character = hardware_character_cache.characters;
		hardware_character_count = MAXIMUM_HARDWARE_CHARACTERS;
		do
		{
			if (hardware_character->character)
				hardware_character->character->hardware_character_index = NONE;
			hardware_character->character = NULL;
			hardware_character++;
		} while (--hardware_character_count);
	}

	return;
}

void
rasterizer_text_cache_dispose(
	void)
{
	if (hardware_character_cache.initialized)
	{
		rasterizer_text_cache_flush();
		bitmap_delete(hardware_character_cache.bitmap);
		hardware_character_cache.initialized = FALSE;
		if (hires_text_atlas)
		{
			text_hires_register_atlas(NULL, 0, 0);
			bitmap_delete(hires_text_atlas);
			hires_text_atlas = NULL;
		}
	}

	return;
}

static void
rasterizer_draw_character(
	struct parse_string_state *state,
	struct font_header *font,
	struct font_character *font_character,
	unsigned long color,
	short x0,
	short y0,
	short x,
	short y,
	short dx,
	short dy)
{
	cache_hardware_format_character(font, font_character);

	if (font_character->hardware_character_index != NONE)
	{
		struct dynamic_screen_vertex vertices[NUMBER_OF_VERTICES_PER_QUADRILATERAL];
		short u0, v0;

		hardware_character_cache_get_origin(font_character->hardware_character_index, &u0, &v0);

		u0 += x;
		v0 += y;

		vertices[0].color = vertices[1].color = vertices[2].color = vertices[3].color = color;

		vertices[0].position.x = vertices[3].position.x = (real)x0;
		vertices[1].position.x = vertices[2].position.x = (real)(x0 + dx);
		vertices[0].position.y = vertices[1].position.y = (real)y0;
		vertices[2].position.y = vertices[3].position.y = (real)(y0 + dy);

		vertices[0].texture_coordinates.x = vertices[3].texture_coordinates.x = (real)u0;
		vertices[1].texture_coordinates.x = vertices[2].texture_coordinates.x = (real)(u0 + dx);
		vertices[0].texture_coordinates.y = vertices[1].texture_coordinates.y = (real)v0;
		vertices[2].texture_coordinates.y = vertices[3].texture_coordinates.y = (real)(v0 + dy);

		rasterizer_text_draw_scaled_character(vertices);
	}

	return;
}

void
rasterizer_draw_string(
	rectangle2d const *bounds,
	rectangle2d const *clip,
	point2d *cursor_reference,
	short height_adjust,
	char const *string)
{
	boolean drop_shadow = TRUE;

	if (rasterizer_debug_options.draw_dynamic_screen_geometry
		&& global_window_parameters.rasterizer_target == _rasterizer_target_render_primary)
	{
		struct bitmap_data *bitmap;
		struct rasterizer_dynamic_screen_geometry_parameters parameters;
		rectangle2d window_bounds;
		rectangle2d viewport_bounds;

		magic_number++;

		match_assert("c:\\halo\\SOURCE\\rasterizer\\rasterizer_text.c", 180, string);

		bitmap = hardware_character_cache_get_bitmap();

		if (bitmap && string[0])
		{
			long length = strlen(string);
			long vertex_count;
			draw_character_proc draw_character;

			if (drop_shadow)
			{
				vertex_count = length * NUMBER_OF_VERTICES_PER_QUADRILATERAL * 2;
				draw_character = rasterizer_draw_character_with_dropshadow;
			}
			else
			{
				vertex_count = length * NUMBER_OF_VERTICES_PER_QUADRILATERAL;
				draw_character = rasterizer_draw_character;
			}

			/* port: from the font's atlas, when it has every character */
			hires_text_font = hires_text_atlas ? hires_text_font_get(draw_string_get_font(), MAX(text_scale, 1.0f)) : NONE;
			if (hires_text_font != NONE)
			{
				long character_index;

				for (character_index = 0; character_index < length; character_index++)
				{
					if (!text_hires_covers(hires_text_font, (unsigned char)string[character_index]))
					{
						hires_text_font = NONE;
						break;
					}
				}
			}
			if (hires_text_font != NONE)
			{
				bitmap = hires_text_atlas;
				draw_character = drop_shadow ?
					rasterizer_draw_hires_character_with_dropshadow :
					rasterizer_draw_hires_character;
			}

			if (!bounds)
			{
				window_bounds = render.camera.window_bounds;
				offset_rectangle2d(
					&window_bounds,
					-render.camera.viewport_bounds.x0,
					-render.camera.viewport_bounds.y0);
			}
			else
			{
				window_bounds = *bounds;
			}

			if (!clip)
			{
				viewport_bounds = render.camera.viewport_bounds;
				offset_rectangle2d(
					&viewport_bounds,
					-render.camera.viewport_bounds.x0,
					-render.camera.viewport_bounds.y0);
			}
			else
			{
				set_rectangle2d(
					&viewport_bounds,
					FLOOR(clip->x0, 0),
					FLOOR(clip->y0, 0),
					MIN(render.camera.viewport_bounds.x1 - render.camera.viewport_bounds.x0, clip->x1),
					MIN(render.camera.viewport_bounds.y1 - render.camera.viewport_bounds.y0, clip->y1));
			}
			/* port: text drawn scaled (rasterizer_text_set_scale) clipped where
			it reaches the viewport once scaled: the clip as it is before the
			scale */
			if (text_scale != 1.0f)
			{
				viewport_bounds.x0 = (short)(text_scale_origin_x + (viewport_bounds.x0 - text_scale_origin_x) / text_scale);
				viewport_bounds.x1 = (short)(text_scale_origin_x + (viewport_bounds.x1 - text_scale_origin_x) / text_scale);
				viewport_bounds.y0 = (short)(text_scale_origin_y + (viewport_bounds.y0 - text_scale_origin_y) / text_scale);
				viewport_bounds.y1 = (short)(text_scale_origin_y + (viewport_bounds.y1 - text_scale_origin_y) / text_scale);
			}

			memset(&parameters, 0, sizeof(parameters));
			parameters.map_texture_scale[0].i = 1.0f / (real)bitmap->width;
			parameters.map_texture_scale[0].j = 1.0f / (real)bitmap->height;
			parameters.map_scale[0].i = parameters.map_scale[0].j = 1.0f;
			parameters.meter_parameters = NULL;
			parameters.point_sampled = FALSE;
			parameters.framebuffer_blend_function = _shader_framebuffer_blend_function_alpha_blend;
			parameters.map[0] = bitmap;

			rasterizer_text_begin(&parameters);
			draw_string(
				draw_character,
				&window_bounds,
				cursor_reference,
				&viewport_bounds,
				height_adjust,
				string);
			rasterizer_text_end();
		}
	}

	return;
}

void
rasterizer_draw_unicode_string(
	rectangle2d const *bounds,
	rectangle2d const *clip,
	point2d *cursor_reference,
	short height_adjust,
	wchar_t const *string)
{
	boolean drop_shadow = TRUE;

	if (rasterizer_debug_options.draw_dynamic_screen_geometry
		&& global_window_parameters.rasterizer_target == _rasterizer_target_render_primary)
	{
		struct bitmap_data *bitmap;
		struct rasterizer_dynamic_screen_geometry_parameters parameters;
		rectangle2d window_bounds;
		rectangle2d viewport_bounds;

		magic_number++;

		match_assert("c:\\halo\\SOURCE\\rasterizer\\rasterizer_text.c", 310, string);

		bitmap = hardware_character_cache_get_bitmap();

		if (bitmap && string[0])
		{
			long length = ustrlen(string);
			long vertex_count;
			draw_character_proc draw_character;

			if (drop_shadow)
			{
				vertex_count = length * NUMBER_OF_VERTICES_PER_QUADRILATERAL * 2;
				draw_character = rasterizer_draw_character_with_dropshadow;
			}
			else
			{
				vertex_count = length * NUMBER_OF_VERTICES_PER_QUADRILATERAL;
				draw_character = rasterizer_draw_character;
			}

			/* port: from the font's atlas, when it has every character */
			hires_text_font = hires_text_atlas ? hires_text_font_get(draw_string_get_font(), MAX(text_scale, 1.0f)) : NONE;
			if (hires_text_font != NONE)
			{
				long character_index;

				for (character_index = 0; character_index < length; character_index++)
				{
					if (!text_hires_covers(hires_text_font, (word)string[character_index]))
					{
						hires_text_font = NONE;
						break;
					}
				}
			}
			if (hires_text_font != NONE)
			{
				bitmap = hires_text_atlas;
				draw_character = drop_shadow ?
					rasterizer_draw_hires_character_with_dropshadow :
					rasterizer_draw_hires_character;
			}

			if (!bounds)
			{
				window_bounds = render.camera.window_bounds;
				offset_rectangle2d(
					&window_bounds,
					-render.camera.viewport_bounds.x0,
					-render.camera.viewport_bounds.y0);
			}
			else
			{
				window_bounds = *bounds;
			}

			if (!clip)
			{
				viewport_bounds = render.camera.viewport_bounds;
				offset_rectangle2d(
					&viewport_bounds,
					-render.camera.viewport_bounds.x0,
					-render.camera.viewport_bounds.y0);
			}
			else
			{
				set_rectangle2d(
					&viewport_bounds,
					FLOOR(clip->x0, 0),
					FLOOR(clip->y0, 0),
					MIN(render.camera.viewport_bounds.x1 - render.camera.viewport_bounds.x0, clip->x1),
					MIN(render.camera.viewport_bounds.y1 - render.camera.viewport_bounds.y0, clip->y1));
			}
			/* port: text drawn scaled (rasterizer_text_set_scale) clipped where
			it reaches the viewport once scaled: the clip as it is before the
			scale */
			if (text_scale != 1.0f)
			{
				viewport_bounds.x0 = (short)(text_scale_origin_x + (viewport_bounds.x0 - text_scale_origin_x) / text_scale);
				viewport_bounds.x1 = (short)(text_scale_origin_x + (viewport_bounds.x1 - text_scale_origin_x) / text_scale);
				viewport_bounds.y0 = (short)(text_scale_origin_y + (viewport_bounds.y0 - text_scale_origin_y) / text_scale);
				viewport_bounds.y1 = (short)(text_scale_origin_y + (viewport_bounds.y1 - text_scale_origin_y) / text_scale);
			}

			memset(&parameters, 0, sizeof(parameters));
			parameters.map_texture_scale[0].i = 1.0f / (real)bitmap->width;
			parameters.map_texture_scale[0].j = 1.0f / (real)bitmap->height;
			parameters.map_scale[0].i = parameters.map_scale[0].j = 1.0f;
			parameters.meter_parameters = NULL;
			parameters.point_sampled = FALSE;
			parameters.framebuffer_blend_function = _shader_framebuffer_blend_function_alpha_blend;
			parameters.map[0] = bitmap;

			rasterizer_text_begin(&parameters);
			draw_unicode_string(
				draw_character,
				&window_bounds,
				cursor_reference,
				&viewport_bounds,
				height_adjust,
				string);
			rasterizer_text_end();
		}
	}

	return;
}

static void
rasterizer_draw_character_with_dropshadow(
	struct parse_string_state *state,
	struct font_header *font,
	struct font_character *font_character,
	unsigned long color,
	short x0,
	short y0,
	short x,
	short y,
	short dx,
	short dy)
{
	cache_hardware_format_character(font, font_character);

	if (font_character->hardware_character_index != NONE)
	{
		struct dynamic_screen_vertex vertices[NUMBER_OF_VERTICES_PER_QUADRILATERAL];
		real x_offset = 1.0f;
		real y_offset = 1.0f;
		unsigned long shadow_color = global_shadow_color
			? global_shadow_color
			: (color & 0xFF000000);
		real left = (real)x0;
		real right = (real)(x0 + dx);
		real top = (real)y0;
		real bottom = (real)(y0 + dy);
		boolean shadow = TRUE;

		while (TRUE)
		{
			unsigned long vertex_color;
			short u0, v0;

			hardware_character_cache_get_origin(font_character->hardware_character_index, &u0, &v0);

			u0 += x;
			v0 += y;

			vertex_color = shadow ? shadow_color : color;

			vertices[0].color = vertices[1].color = vertices[2].color = vertices[3].color = vertex_color;

			vertices[0].position.x = vertices[3].position.x = left + x_offset;
			vertices[1].position.x = vertices[2].position.x = right + x_offset;
			vertices[0].position.y = vertices[1].position.y = top + y_offset;
			vertices[2].position.y = vertices[3].position.y = bottom + y_offset;

			vertices[0].texture_coordinates.x = vertices[3].texture_coordinates.x = (real)u0;
			vertices[1].texture_coordinates.x = vertices[2].texture_coordinates.x = (real)(u0 + dx);
			vertices[0].texture_coordinates.y = vertices[1].texture_coordinates.y = (real)v0;
			vertices[2].texture_coordinates.y = vertices[3].texture_coordinates.y = (real)(v0 + dy);

			rasterizer_text_draw_scaled_character(vertices);

			if (!shadow)
				break;

			shadow = FALSE;
			x_offset = y_offset = 0.0f;
		}
	}

	return;
}

/* ---------- private code */

/* port: text drawn scale times larger about a point (in screen units), until
it is set back to 1: the characters' quads are laid out as before, and
scaled about it as they are drawn */
void rasterizer_text_set_scale(
	real scale,
	real origin_x,
	real origin_y)
{
	text_scale = scale > 0.0f ? scale : 1.0f;
	text_scale_origin_x = origin_x;
	text_scale_origin_y = origin_y;

	return;
}

/* port: a character's quad, scaled (rasterizer_text_set_scale) */
static void rasterizer_text_draw_scaled_character(
	struct dynamic_screen_vertex const *vertices)
{
	struct dynamic_screen_vertex scaled[NUMBER_OF_VERTICES_PER_QUADRILATERAL];
	short vertex_index;

	if (text_scale == 1.0f)
	{
		rasterizer_text_draw_character(vertices);
		return;
	}
	for (vertex_index = 0; vertex_index < NUMBER_OF_VERTICES_PER_QUADRILATERAL; vertex_index++)
	{
		scaled[vertex_index] = vertices[vertex_index];
		scaled[vertex_index].position.x =
			text_scale_origin_x + (vertices[vertex_index].position.x - text_scale_origin_x) * text_scale;
		scaled[vertex_index].position.y =
			text_scale_origin_y + (vertices[vertex_index].position.y - text_scale_origin_y) * text_scale;
	}
	rasterizer_text_draw_character(scaled);

	return;
}

/* port: the high-res text's font for a font tag, sized by the height of its
capital H (its rows with ink), its glyphs drawn with oversample times the
pixels (for text scaled up), or NONE */
static long hires_text_font_get(
	long font_index,
	real oversample)
{
	static struct
	{
		struct font_header *font;
		real oversample;
		long hires_font;
	} fonts[MAXIMUM_HIRES_TEXT_FONTS];
	static short next_font = 0;
	struct font_header *font;
	struct font_character *capital;
	short font_slot;
	short top = NONE;
	short bottom = NONE;
	short row;
	long hires_font;

	if (font_index == NONE)
		return NONE;
	font = font_definition_get(font_index);
	for (font_slot = 0; font_slot < MAXIMUM_HIRES_TEXT_FONTS; font_slot++)
	{
		if (fonts[font_slot].font == font && fonts[font_slot].oversample == oversample)
			return fonts[font_slot].hires_font;
	}
	capital = font_get_character_by_ascii_code(font, 'H');
	if (capital)
	{
		byte const *pixels = (byte const *)font->pixels.address + capital->pixels_offset;

		for (row = 0; row < capital->bitmap_height; row++)
		{
			short column;

			for (column = 0; column < capital->bitmap_width; column++)
			{
				if (pixels[row * capital->bitmap_width + column])
				{
					if (top == NONE)
						top = row;
					bottom = row;
					break;
				}
			}
		}
	}
	hires_font = top == NONE ? NONE :
		text_hires_font(tag_get_name(font_index), (float)(bottom - top + 1), (float)oversample);
	if (hires_font < 0)
		hires_font = NONE;
	fonts[next_font].font = font;
	fonts[next_font].oversample = oversample;
	fonts[next_font].hires_font = hires_font;
	next_font = (short)((next_font + 1) % MAXIMUM_HIRES_TEXT_FONTS);

	return hires_font;
}

/* port: the font tag's character's ink in its bitmap's columns x0 to x1 and
rows y0 to y1 (its pixels' coverage, added) */
static long font_character_ink(
	struct font_header *font,
	struct font_character *font_character,
	short x0,
	short x1,
	short y0,
	short y1)
{
	byte const *pixels = (byte const *)font->pixels.address + font_character->pixels_offset;
	short row, column;
	long ink = 0;

	for (row = MAX(y0, 0); row < MIN(y1, font_character->bitmap_height); row++)
	{
		for (column = MAX(x0, 0); column < MIN(x1, font_character->bitmap_width); column++)
			ink += pixels[row * font_character->bitmap_width + column];
	}

	return ink;
}

/* port: whether draw_string's cut of the font tag's character took a part
of it that shows: more than an eighth of its ink (a cut through its edge's
faint pixels, or its bitmap's empty columns, leaves it whole to the eye) */
static boolean font_character_cut(
	struct font_header *font,
	struct font_character *font_character,
	short x0,
	short x1,
	short y0,
	short y1)
{
	long all = font_character_ink(font, font_character, 0, font_character->bitmap_width, 0, font_character->bitmap_height);

	return font_character_ink(font, font_character, x0, x1, y0, y1) * 8 > all;
}

/* port: a character from the high-res text's atlas: the font's glyph on the
baseline, its advance centred on the font tag's character's, cut where
draw_string cut a part of the font tag's character that shows (to the
text's box). Where it cut only the character's bitmap's empty columns or
rows, or its edge's faint pixels, the character looked whole, and so does
the glyph. */
static void rasterizer_draw_hires_glyph(
	struct font_header *font,
	struct font_character *font_character,
	unsigned long color,
	unsigned long shadow_color,
	boolean shadow,
	short x0,
	short y0,
	short x,
	short y,
	short dx,
	short dy)
{
	struct text_hires_glyph glyph;
	struct dynamic_screen_vertex vertices[NUMBER_OF_VERTICES_PER_QUADRILATERAL];
	real pen_x = (real)(x0 - x + font_character->bitmap_origin_x);
	real baseline = (real)(y0 - y + font_character->bitmap_origin_y);
	real clip_left = x > 0 ? (real)x0 : -32768.0f;
	real clip_top = y > 0 ? (real)y0 : -32768.0f;
	real clip_right = x + dx < font_character->bitmap_width ? (real)(x0 + dx) : 32767.0f;
	real clip_bottom = y + dy < font_character->bitmap_height ? (real)(y0 + dy) : 32767.0f;
	real left, top, right, bottom, u0, v0, u1, v1;
	short pass;

	if (!text_hires_glyph(hires_text_font, font_character->character, &glyph))
		return;
	/* (the font's advance centred on the tag's, so that its letters keep
	their own spacing) */
	left = pen_x + (font_character->character_width - glyph.advance) * 0.5f + glyph.left;
	right = left + (glyph.right - glyph.left);
	if (x > 0 && !font_character_cut(font, font_character, 0, x, 0, font_character->bitmap_height))
		clip_left = -32768.0f;
	if (x + dx < font_character->bitmap_width &&
		!font_character_cut(font, font_character, x + dx, font_character->bitmap_width, 0, font_character->bitmap_height))
	{
		clip_right = 32767.0f;
	}
	if (y > 0 && !font_character_cut(font, font_character, 0, font_character->bitmap_width, 0, y))
		clip_top = -32768.0f;
	if (y + dy < font_character->bitmap_height &&
		!font_character_cut(font, font_character, 0, font_character->bitmap_width, y + dy, font_character->bitmap_height))
	{
		clip_bottom = 32767.0f;
	}
	top = baseline + glyph.top;
	bottom = baseline + glyph.bottom;
	u0 = glyph.u0;
	u1 = glyph.u1;
	v0 = glyph.v0;
	v1 = glyph.v1;
	if (left < clip_left)
	{
		u0 += (clip_left - left) * (u1 - u0) / (right - left);
		left = clip_left;
	}
	if (right > clip_right)
	{
		u1 -= (right - clip_right) * (u1 - u0) / (right - left);
		right = clip_right;
	}
	if (top < clip_top)
	{
		v0 += (clip_top - top) * (v1 - v0) / (bottom - top);
		top = clip_top;
	}
	if (bottom > clip_bottom)
	{
		v1 -= (bottom - clip_bottom) * (v1 - v0) / (bottom - top);
		bottom = clip_bottom;
	}
	if (right <= left || bottom <= top)
		return;

	/* (the drop shadow first, a unit down and right, as the font tags') */
	for (pass = shadow ? 0 : 1; pass < 2; pass++)
	{
		real offset = pass ? 0.0f : 1.0f;
		unsigned long vertex_color = pass ? color : shadow_color;

		vertices[0].color = vertices[1].color = vertices[2].color = vertices[3].color = vertex_color;
		vertices[0].position.x = vertices[3].position.x = left + offset;
		vertices[1].position.x = vertices[2].position.x = right + offset;
		vertices[0].position.y = vertices[1].position.y = top + offset;
		vertices[2].position.y = vertices[3].position.y = bottom + offset;
		vertices[0].texture_coordinates.x = vertices[3].texture_coordinates.x = u0;
		vertices[1].texture_coordinates.x = vertices[2].texture_coordinates.x = u1;
		vertices[0].texture_coordinates.y = vertices[1].texture_coordinates.y = v0;
		vertices[2].texture_coordinates.y = vertices[3].texture_coordinates.y = v1;
		rasterizer_text_draw_scaled_character(vertices);
	}

	return;
}

static void rasterizer_draw_hires_character(
	struct parse_string_state *state,
	struct font_header *font,
	struct font_character *font_character,
	unsigned long color,
	short x0,
	short y0,
	short x,
	short y,
	short dx,
	short dy)
{
	rasterizer_draw_hires_glyph(font, font_character, color, 0, FALSE, x0, y0, x, y, dx, dy);

	return;
}

static void rasterizer_draw_hires_character_with_dropshadow(
	struct parse_string_state *state,
	struct font_header *font,
	struct font_character *font_character,
	unsigned long color,
	short x0,
	short y0,
	short x,
	short y,
	short dx,
	short dy)
{
	unsigned long shadow_color = global_shadow_color ?
		global_shadow_color :
		(color & 0xFF000000);

	rasterizer_draw_hires_glyph(font, font_character, color, shadow_color, TRUE, x0, y0, x, y, dx, dy);

	return;
}

static struct bitmap_data *
hardware_character_cache_get_bitmap(
	void)
{
	return hardware_character_cache.initialized ? hardware_character_cache.bitmap : NULL;
}

static void
hardware_character_cache_get_origin(
	short hardware_character_index,
	short *x0,
	short *y0)
{
	struct hardware_character *hardware_character =
		&hardware_character_cache.characters[hardware_character_index];

	match_assert("c:\\halo\\SOURCE\\rasterizer\\rasterizer_text.c", 597, hardware_character_cache.initialized);
	match_assert("c:\\halo\\SOURCE\\rasterizer\\rasterizer_text.c", 598, hardware_character_index>=0 && hardware_character_index<MAXIMUM_HARDWARE_CHARACTERS);
	match_assert("c:\\halo\\SOURCE\\rasterizer\\rasterizer_text.c", 599, x0 && y0);

	*x0 = hardware_character->x0 + hardware_character_padding(hardware_character->character);
	*y0 = hardware_character->y0 + hardware_character_padding(hardware_character->character);

	return;
}

/* port: the clear texels around a character in the cache (its cell is that
much larger each side). The cache packs characters edge to edge, which the
Xbox's 640x480 sampled texel for pixel; drawn larger (a fullscreen
display's resolution), each edge pixel blends in half a texel past the
character, from the next character or one the cache dropped, and the text
and its drop shadow showed the cells' borders. A character too large for
the border goes without. */
static short hardware_character_padding(
	struct font_character const *font_character)
{
	return font_character &&
		font_character->bitmap_width + 2 <= HARDWARE_CHARACTER_CACHE_BITMAP_WIDTH &&
		font_character->bitmap_height + 2 <= HARDWARE_CHARACTER_CACHE_BITMAP_HEIGHT ? 1 : 0;
}

static void
flush_hardware_character(
	struct hardware_character *hardware_character)
{
	match_assert("c:\\halo\\SOURCE\\rasterizer\\rasterizer_text.c", 610, hardware_character);

	if (hardware_character->character)
	{
		hardware_character->character->hardware_character_index = NONE;

		if (hardware_character->character->pad == magic_number)
			error(_error_log, "font cache overwrote character in use");

		hardware_character->character = NULL;
	}

	return;
}

static void
cache_hardware_format_character(
	struct font_header *font,
	struct font_character *font_character)
{
	match_assert("c:\\halo\\SOURCE\\rasterizer\\rasterizer_text.c", 633, hardware_character_cache.initialized);

	if (font_character->hardware_character_index != NONE)
	{
		match_assert("c:\\halo\\SOURCE\\rasterizer\\rasterizer_text.c", 637, font_character->hardware_character_index>=0 && font_character->hardware_character_index<MAXIMUM_HARDWARE_CHARACTERS);
		match_assert("c:\\halo\\SOURCE\\rasterizer\\rasterizer_text.c", 638, font_character==hardware_character_cache.characters[font_character->hardware_character_index].character);
	}
	else
	{
		struct hardware_character *hardware_character;
		byte *source;
		short y0, y1;
		short x, y;
		short next_write_index;
		short padding = hardware_character_padding(font_character);
		short cell_width = font_character->bitmap_width + 2 * padding;
		short cell_height = font_character->bitmap_height + 2 * padding;

		match_assert("c:\\halo\\SOURCE\\rasterizer\\rasterizer_text.c", 645, font_character->bitmap_width<=HARDWARE_CHARACTER_CACHE_BITMAP_WIDTH);
		match_assert("c:\\halo\\SOURCE\\rasterizer\\rasterizer_text.c", 646, font_character->bitmap_height<=HARDWARE_CHARACTER_CACHE_BITMAP_HEIGHT);

		font_character->pad = magic_number;

		if (cell_width + hardware_character_cache.x0 > HARDWARE_CHARACTER_CACHE_BITMAP_WIDTH)
		{
			hardware_character_cache.x0 = 0;
			hardware_character_cache.y0 += hardware_character_cache.maximum_character_height;
			hardware_character_cache.maximum_character_height = 0;
		}

		if (cell_height + hardware_character_cache.y0 > HARDWARE_CHARACTER_CACHE_BITMAP_HEIGHT)
		{
			hardware_character_cache.y0 = 0;
			hardware_character_cache.x0 = 0;
			hardware_character_cache.maximum_character_height = 0;

			for (;
				hardware_character_cache.read_index != hardware_character_cache.write_index;
				hardware_character_cache.read_index = (hardware_character_cache.read_index + 1) & (MAXIMUM_HARDWARE_CHARACTERS - 1))
			{
				hardware_character = &hardware_character_cache.characters[hardware_character_cache.read_index];

				if (hardware_character->y0 <= 0)
					break;

				flush_hardware_character(hardware_character);
			}
		}

		if (cell_height > hardware_character_cache.maximum_character_height)
		{
			y0 = hardware_character_cache.y0 + hardware_character_cache.maximum_character_height;
			y1 = hardware_character_cache.y0 + cell_height;

			for (;
				hardware_character_cache.read_index != hardware_character_cache.write_index;
				hardware_character_cache.read_index = (hardware_character_cache.read_index + 1) & (MAXIMUM_HARDWARE_CHARACTERS - 1))
			{
				hardware_character = &hardware_character_cache.characters[hardware_character_cache.read_index];

				if (hardware_character->y0 < y0 || hardware_character->y0 >= y1)
					break;

				flush_hardware_character(hardware_character);
			}

			hardware_character_cache.maximum_character_height = cell_height;
		}

		next_write_index = (hardware_character_cache.write_index + 1) & (MAXIMUM_HARDWARE_CHARACTERS - 1);
		if (next_write_index == hardware_character_cache.read_index)
		{
			flush_hardware_character(&hardware_character_cache.characters[hardware_character_cache.read_index]);
			hardware_character_cache.read_index = (hardware_character_cache.read_index + 1) & (MAXIMUM_HARDWARE_CHARACTERS - 1);
		}

		hardware_character = &hardware_character_cache.characters[hardware_character_cache.write_index];
		font_character->hardware_character_index = hardware_character_cache.write_index;

		hardware_character->character = font_character;
		hardware_character->x0 = hardware_character_cache.x0;
		hardware_character->y0 = hardware_character_cache.y0;

		source = (byte *)font->pixels.address + font_character->pixels_offset;

		/* (port: the border clear, white with no alpha as the character's
		own clear texels are) */
		for (y = 0; y < cell_height; y++)
		{
			word *destination = (word *)bitmap_2d_address(
				hardware_character_cache.bitmap,
				hardware_character->x0,
				(short)(hardware_character->y0 + y),
				0);

			for (x = 0; x < cell_width; x++)
			{
				if (y < padding || y >= padding + font_character->bitmap_height ||
					x < padding || x >= padding + font_character->bitmap_width)
				{
					*destination++ = 0x0FFF;
				}
				else
				{
					*destination++ = (word)((*source++ << 8) | 0x0FFF);
				}
			}
		}

		rasterizer_bitmap_changed(hardware_character_cache.bitmap);

		hardware_character_cache.x0 += cell_width;
		hardware_character_cache.write_index = (hardware_character_cache.write_index + 1) & (MAXIMUM_HARDWARE_CHARACTERS - 1);
	}

	return;
}
