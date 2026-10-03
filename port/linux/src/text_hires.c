/*
TEXT_HIRES.C

The game's text drawn with fonts at the display's resolution (text_hires.h).

Each font tag the fonts draw (port/assets/fonts/fonts.json) gets its font
sized so that its capitals are as tall as the tag's: the game's layout,
from the tag's character widths, is kept, and the fonts were chosen to fit
those widths. A glyph is rasterized when first drawn, at the display's
pixels per unit of the 480 lines, into an 8-bit atlas; the GL texture the
game's placeholder bitmap stands for (xbox_textures.c) is the atlas, its new
rows uploaded when the placeholder is bound. A change of the display's
scale (a window made fullscreen) or a full atlas starts it over.
*/

#include "text_hires.h"
#include "platform.h"
#include "port_config.h"
#include "xgpu.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "../../third_party/stb/stb_truetype.h"

#define MAXIMUM_FONTS 8
#define ATLAS_SIZE 2048
/* clear texels around each glyph, so that filtering never reaches the next */
#define ATLAS_PADDING 2
#define GLYPH_CACHE_SIZE 4096

static struct
{
	char tag[64];
	stbtt_fontinfo info;
	/* font units to units of the 480 lines */
	float scale;
	/* how many times the display's pixels its glyphs are rasterized with */
	float oversample;
} fonts[MAXIMUM_FONTS];
static long font_count;

static struct
{
	long font;          /* -1: free */
	unsigned long code;
	short x, y, width, height;
	short left, top;    /* the glyph's corner, in pixels from the pen on the baseline */
	float advance;      /* in units of the 480 lines */
} glyphs[GLYPH_CACHE_SIZE];

static unsigned char *atlas;
static float atlas_scale;   /* the pixels per unit its glyphs have */
static int pack_x, pack_y, pack_row_height;
static int dirty_top = ATLAS_SIZE, dirty_bottom;
static unsigned long placeholder_data, placeholder_width, placeholder_height;
static GLuint atlas_texture;

/* tag names compared as the game does, ignoring case (and without the POSIX
strcasecmp, which the Windows build has not) */
static int names_differ(const char *a, const char *b)
{
	while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b))
	{
		a++;
		b++;
	}
	return tolower((unsigned char)*a) != tolower((unsigned char)*b);
}

static int text_enabled(void)
{
	static int enabled;
	static unsigned long read_at = (unsigned long)-1;

	if (read_at != config_changes())
	{
		read_at = config_changes();
		enabled = config_boolean("display.high_res_text");
	}
	return enabled;
}

/* the atlas emptied: every glyph to be rasterized again */
static void atlas_reset(float scale)
{
	long index;

	if (!atlas)
		atlas = malloc(ATLAS_SIZE * ATLAS_SIZE);
	if (atlas)
		memset(atlas, 0, ATLAS_SIZE * ATLAS_SIZE);
	for (index = 0; index < GLYPH_CACHE_SIZE; index++)
		glyphs[index].font = -1;
	atlas_scale = scale;
	pack_x = pack_y = pack_row_height = 0;
	dirty_top = 0;
	dirty_bottom = ATLAS_SIZE;
}

long text_hires_font(char const *tag_name, float cap_height, float oversample)
{
	unsigned int index;
	long font;
	int x0, y0, x1, y1;

	if (!text_enabled() || !tag_name || cap_height <= 0.0f)
		return -1;
	/* (text is never drawn with fewer pixels than the display's) */
	if (oversample < 1.0f)
		oversample = 1.0f;
	for (font = 0; font < font_count; font++)
	{
		if (!names_differ(fonts[font].tag, tag_name) && fonts[font].oversample == oversample)
			return fonts[font].scale > 0.0f ? font : -1;
	}
	if (font_count >= MAXIMUM_FONTS)
		return -1;
	font = font_count++;
	snprintf(fonts[font].tag, sizeof(fonts[font].tag), "%s", tag_name);
	fonts[font].scale = 0.0f;
	fonts[font].oversample = oversample;
	for (index = 0; index < text_hires_embedded_count; index++)
	{
		const struct text_hires_embedded *embedded = &text_hires_embedded[index];
		const unsigned char *data = (const unsigned char *)embedded->data;

		if (!embedded->tag || names_differ(embedded->tag, tag_name))
			continue;
		if (!stbtt_InitFont(&fonts[font].info, data, stbtt_GetFontOffsetForIndex(data, 0)) ||
			!stbtt_GetCodepointBox(&fonts[font].info, 'H', &x0, &y0, &x1, &y1) || y1 <= y0)
		{
			platform_log("high-res text: could not read %s for %s", embedded->file, tag_name);
			break;
		}
		fonts[font].scale = cap_height / (float)(y1 - y0);
		platform_log("high-res text: %s drawn with %s", tag_name, embedded->file);
		break;
	}
	return fonts[font].scale > 0.0f ? font : -1;
}

int text_hires_covers(long font, unsigned long code)
{
	if (font < 0 || font >= font_count || fonts[font].scale <= 0.0f)
		return 0;
	/* (control characters are never drawn) */
	return code < 32 || code == ' ' || stbtt_FindGlyphIndex(&fonts[font].info, (int)code) != 0;
}

int text_hires_glyph(long font, unsigned long code, struct text_hires_glyph *glyph)
{
	float scale = halo_screen_pixel_scale();
	float pixel_scale;
	unsigned long slot;
	long probe;

	if (font < 0 || font >= font_count || fonts[font].scale <= 0.0f || !placeholder_data || scale <= 0.0f)
		return 0;
	if (!atlas || scale != atlas_scale)
		atlas_reset(scale);
	if (!atlas)
		return 0;
	slot = ((unsigned long)font * 2654435761UL ^ code * 40503UL) & (GLYPH_CACHE_SIZE - 1);
	for (probe = 0; probe < GLYPH_CACHE_SIZE; probe++, slot = (slot + 1) & (GLYPH_CACHE_SIZE - 1))
	{
		if (glyphs[slot].font == -1 || (glyphs[slot].font == font && glyphs[slot].code == code))
			break;
	}
	if (probe == GLYPH_CACHE_SIZE)
	{
		atlas_reset(scale);
		return text_hires_glyph(font, code, glyph);
	}
	if (glyphs[slot].font == -1)
	{
		/* rasterized now */
		float pixels = fonts[font].scale * scale * fonts[font].oversample;
		int index = stbtt_FindGlyphIndex(&fonts[font].info, (int)code);
		int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
		int width, height, advance = 0, bearing = 0;

		if (index)
		{
			stbtt_GetGlyphBitmapBox(&fonts[font].info, index, pixels, pixels, &x0, &y0, &x1, &y1);
			stbtt_GetGlyphHMetrics(&fonts[font].info, index, &advance, &bearing);
		}
		width = x1 - x0;
		height = y1 - y0;
		if (width > 0 && height > 0)
		{
			if (width + 2 * ATLAS_PADDING > ATLAS_SIZE || height + 2 * ATLAS_PADDING > ATLAS_SIZE)
				return 0;
			if (pack_x + width + 2 * ATLAS_PADDING > ATLAS_SIZE)
			{
				pack_x = 0;
				pack_y += pack_row_height;
				pack_row_height = 0;
			}
			if (pack_y + height + 2 * ATLAS_PADDING > ATLAS_SIZE)
			{
				atlas_reset(scale);
				return text_hires_glyph(font, code, glyph);
			}
			stbtt_MakeGlyphBitmap(&fonts[font].info,
				atlas + (pack_y + ATLAS_PADDING) * ATLAS_SIZE + pack_x + ATLAS_PADDING,
				width, height, ATLAS_SIZE, pixels, pixels, index);
			if (pack_y < dirty_top)
				dirty_top = pack_y;
			if (pack_y + height + 2 * ATLAS_PADDING > dirty_bottom)
				dirty_bottom = pack_y + height + 2 * ATLAS_PADDING;
		}
		glyphs[slot].font = font;
		glyphs[slot].code = code;
		glyphs[slot].x = (short)(pack_x + ATLAS_PADDING);
		glyphs[slot].y = (short)(pack_y + ATLAS_PADDING);
		glyphs[slot].width = (short)(width > 0 && height > 0 ? width : 0);
		glyphs[slot].height = (short)(width > 0 && height > 0 ? height : 0);
		glyphs[slot].left = (short)x0;
		glyphs[slot].top = (short)y0;
		glyphs[slot].advance = advance * fonts[font].scale;
		if (width > 0 && height > 0)
		{
			pack_x += width + 2 * ATLAS_PADDING;
			if (height + 2 * ATLAS_PADDING > pack_row_height)
				pack_row_height = height + 2 * ATLAS_PADDING;
		}
	}
	if (!glyphs[slot].width || !glyphs[slot].height)
		return 0;
	pixel_scale = scale * fonts[font].oversample;
	glyph->left = glyphs[slot].left / pixel_scale;
	glyph->top = glyphs[slot].top / pixel_scale;
	glyph->right = (glyphs[slot].left + glyphs[slot].width) / pixel_scale;
	glyph->bottom = (glyphs[slot].top + glyphs[slot].height) / pixel_scale;
	glyph->advance = glyphs[slot].advance;
	glyph->u0 = (float)glyphs[slot].x * placeholder_width / ATLAS_SIZE;
	glyph->v0 = (float)glyphs[slot].y * placeholder_height / ATLAS_SIZE;
	glyph->u1 = (float)(glyphs[slot].x + glyphs[slot].width) * placeholder_width / ATLAS_SIZE;
	glyph->v1 = (float)(glyphs[slot].y + glyphs[slot].height) * placeholder_height / ATLAS_SIZE;
	return 1;
}

void text_hires_register_atlas(const unsigned long *texture, unsigned long width, unsigned long height)
{
	placeholder_data = texture ? texture[1] : 0;
	placeholder_width = width;
	placeholder_height = height;
}

unsigned int text_hires_atlas_texture(unsigned long data)
{
	if (!data || data != placeholder_data || !atlas)
		return 0;
	if (!atlas_texture)
	{
		glGenTextures(1, &atlas_texture);
		glBindTexture(GL_TEXTURE_2D, atlas_texture);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ATLAS_SIZE, ATLAS_SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		xgpu_gl_state_invalidate();
		dirty_top = 0;
		dirty_bottom = ATLAS_SIZE;
	}
	if (dirty_top < dirty_bottom)
	{
		/* (white, the glyph's coverage its alpha, as the maps' fonts are) */
		long rows = dirty_bottom - dirty_top;
		unsigned char *texels = malloc((size_t)rows * ATLAS_SIZE * 4);

		if (texels)
		{
			long index;
			const unsigned char *coverage = atlas + (long)dirty_top * ATLAS_SIZE;

			for (index = 0; index < rows * ATLAS_SIZE; index++)
			{
				texels[index * 4 + 0] = texels[index * 4 + 1] = texels[index * 4 + 2] = 255;
				texels[index * 4 + 3] = coverage[index];
			}
			glBindTexture(GL_TEXTURE_2D, atlas_texture);
			glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, dirty_top, ATLAS_SIZE, (GLsizei)rows, GL_RGBA, GL_UNSIGNED_BYTE, texels);
			xgpu_gl_state_invalidate();
			free(texels);
			dirty_top = ATLAS_SIZE;
			dirty_bottom = 0;
		}
	}
	return atlas_texture;
}
