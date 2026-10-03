/*
HUD_HIRES.C

The high-res HUD's textures (hud_hires.h): which one stands for a bitmap being
uploaded, and each one's GL texture.

Which bitmap is at an address the game knows (from the loaded map's tags:
port/linux/game/hud_hires_tags.c). Each texture is decoded from its PNG when
first drawn and kept: up to 69 of the HUD's, about 225 MB with their mip
levels, though a game draws only some (the scopes' only when zoomed), and
the titles of the menus shown, about 3 MB each (11 MB for the carnage
report's, a whole panel), and the profiles' Spartan pictures shown, about
0.7 MB each.
They are drawn with linear filtering and their mip levels (d3d8_gl.c,
configure_sampler), as they are larger than they appear.

The PNGs are the ones tools/hud_assets.py, title_assets.py and
spartan_assets.py write, so only what they write is read: 8-bit RGBA, not
interlaced, its data inflated with the game's zlib.
*/

#include "hud_hires.h"
#include "platform.h"
#include "port_config.h"
#include "xgpu.h"

#include "memory/zlib/zlib.h"

#include <stdlib.h>
#include <string.h>

/* the game's (port/linux/game/hud_hires_tags.c) */
long hud_hires_asset_at(unsigned long address, long width, long height);

static struct
{
	unsigned int texture;
	unsigned long levels;
	int failed;
	int other_pixels_logged;
} textures[HUD_HIRES_MAXIMUM_TEXTURES];

long hud_hires_asset_count(void)
{
	return hud_hires_embedded_count < HUD_HIRES_MAXIMUM_TEXTURES ? (long)hud_hires_embedded_count :
		HUD_HIRES_MAXIMUM_TEXTURES;
}

char const *hud_hires_asset_tag(long asset)
{
	return hud_hires_embedded[asset].tag;
}

long hud_hires_asset_bitmap(long asset)
{
	return hud_hires_embedded[asset].bitmap;
}

/* whether the texture can stand for a bitmap of this size: a whole multiple
of it, the same both ways */
long hud_hires_asset_fits(long asset, long width, long height)
{
	const struct hud_hires_embedded *embedded = &hud_hires_embedded[asset];

	return width > 0 && height > 0 && embedded->width % width == 0 && embedded->height % height == 0 &&
		embedded->width / width == embedded->height / height && embedded->width / width > 1;
}

long hud_hires_override_find(unsigned long address, unsigned long width, unsigned long height,
	unsigned long level0_size)
{
	static int hud_enabled = -1, titles_enabled = -1;
	long asset;

	if (hud_enabled < 0)
	{
		hud_enabled = config_boolean("display.high_res_hud");
		titles_enabled = config_boolean("display.high_res_text");
	}
	if (!hud_enabled && !titles_enabled)
		return -1;
	asset = hud_hires_asset_at(address, (long)width, (long)height);
	if (asset < 0 || asset >= hud_hires_asset_count())
		return -1;
	if (!(hud_hires_embedded[asset].title ? titles_enabled : hud_enabled))
		return -1;
	if (crc32(0L, (const Bytef *)address, (uInt)level0_size) != hud_hires_embedded[asset].crc)
	{
		if (!textures[asset].other_pixels_logged)
		{
			platform_log("high-res hud: %s bitmap %d is not the one its texture was drawn for here "
				"(another language's or a modified map): drawn as it is",
				hud_hires_embedded[asset].tag, hud_hires_embedded[asset].bitmap);
			textures[asset].other_pixels_logged = 1;
		}
		return -1;
	}
	return asset;
}

int hud_hires_override_coverage(long asset)
{
	return asset >= 0 && asset < hud_hires_asset_count() && hud_hires_embedded[asset].coverage;
}

/* ---------- decoding */

static unsigned long big_endian_long(const unsigned char *bytes)
{
	return ((unsigned long)bytes[0] << 24) | ((unsigned long)bytes[1] << 16) |
		((unsigned long)bytes[2] << 8) | bytes[3];
}

static unsigned char paeth(unsigned char left, unsigned char up, unsigned char up_left)
{
	int estimate = (int)left + up - up_left;
	int to_left = abs(estimate - left), to_up = abs(estimate - up), to_up_left = abs(estimate - up_left);

	if (to_left <= to_up && to_left <= to_up_left)
		return left;
	return to_up <= to_up_left ? up : up_left;
}

/* the PNG's texels, RGBA in rows top first; NULL if it is not one that
tools/hud_assets.py writes */
static unsigned char *png_decode(const struct hud_hires_embedded *embedded)
{
	const unsigned char *data = (const unsigned char *)embedded->png;
	unsigned long size = embedded->png_size, position = 8;
	unsigned long width = embedded->width, height = embedded->height;
	unsigned long stride = width * 4, filtered_size = height * (stride + 1);
	unsigned char *compressed = NULL, *filtered = NULL, *pixels = NULL;
	unsigned long compressed_size = 0, row, column;
	uLongf inflated_size = filtered_size;
	int result;

	if (size < 33 || memcmp(data, "\x89PNG\r\n\x1a\n", 8) || memcmp(data + 12, "IHDR", 4) ||
		big_endian_long(data + 16) != width || big_endian_long(data + 20) != height ||
		data[24] != 8 || data[25] != 6 || data[28] != 0)
		return NULL;
	compressed = malloc(size);
	while (compressed && position + 12 <= size)
	{
		unsigned long length = big_endian_long(data + position);

		if (length > size - position - 12)
			break;
		if (!memcmp(data + position + 4, "IDAT", 4))
		{
			memcpy(compressed + compressed_size, data + position + 8, length);
			compressed_size += length;
		}
		else if (!memcmp(data + position + 4, "IEND", 4))
		{
			break;
		}
		position += 12 + length;
	}
	filtered = compressed ? malloc(filtered_size) : NULL;
	pixels = filtered ? malloc(stride * height) : NULL;
	if (!pixels)
		goto failed;
	result = uncompress(filtered, &inflated_size, compressed, compressed_size);
	/* (the game's zlib is 1.1, which can stop short of saying the stream has
	ended when the output is exactly full: all of it is enough) */
	if ((result != Z_OK && result != Z_BUF_ERROR) || inflated_size != filtered_size)
		goto failed;
	for (row = 0; row < height; row++)
	{
		const unsigned char *line = filtered + row * (stride + 1) + 1;
		unsigned char filter = line[-1];
		unsigned char *out = pixels + row * stride;
		const unsigned char *above = row ? out - stride : NULL;

		if (filter > 4)
			goto failed;
		for (column = 0; column < stride; column++)
		{
			unsigned char left = column >= 4 ? out[column - 4] : 0;
			unsigned char up = above ? above[column] : 0;
			unsigned char up_left = above && column >= 4 ? above[column - 4] : 0;
			unsigned char predicted;

			switch (filter)
			{
			case 1: predicted = left; break;
			case 2: predicted = up; break;
			case 3: predicted = (unsigned char)(((unsigned)left + up) / 2); break;
			case 4: predicted = paeth(left, up, up_left); break;
			default: predicted = 0; break;
			}
			out[column] = (unsigned char)(line[column] + predicted);
		}
	}
	free(compressed);
	free(filtered);
	return pixels;

failed:
	free(compressed);
	free(filtered);
	free(pixels);
	return NULL;
}

unsigned int hud_hires_override_texture(long asset, unsigned long *levels)
{
	const struct hud_hires_embedded *embedded;
	unsigned char *pixels;
	unsigned long largest;
	GLuint texture;

	if (asset < 0 || asset >= hud_hires_asset_count() || textures[asset].failed)
		return 0;
	if (textures[asset].texture)
	{
		*levels = textures[asset].levels;
		return textures[asset].texture;
	}
	embedded = &hud_hires_embedded[asset];
	pixels = png_decode(embedded);
	if (!pixels)
	{
		platform_log("high-res hud: could not decode the texture for %s bitmap %d", embedded->tag, embedded->bitmap);
		textures[asset].failed = 1;
		return 0;
	}
	textures[asset].levels = 1;
	for (largest = embedded->width > embedded->height ? embedded->width : embedded->height; largest > 1; largest >>= 1)
		textures[asset].levels++;
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	xgpu_gl_state_invalidate();
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)textures[asset].levels - 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)embedded->width, (GLsizei)embedded->height, 0,
		GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	glGenerateMipmap(GL_TEXTURE_2D);
	xgpu_gl_state_invalidate();
	free(pixels);
	textures[asset].texture = texture;
	*levels = textures[asset].levels;
	return texture;
}
