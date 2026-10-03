/*
HUD_HIRES.H

The high-res HUD: textures drawn from the hand-made SVG redraws of the Halo PC
HUD sheets (port/assets/hud, made by tools/hud_assets.py), each 8x the size
of the bitmap of the Xbox maps it stands for (4x for the largest) and in that
bitmap's layout. The menus' titles, pictures of text in the maps, are drawn
the same way (port/assets/titles, made by tools/title_assets.py).
tools/embed_assets.py makes them C data (hud_hires_embedded); hud_hires.c
decodes them, and the texture cache (xbox_textures.c) draws one in place of
its bitmap whenever that bitmap's pixels are uploaded: the game still sizes
and places the bitmap by its tag, so nothing else changes.
*/

#ifndef HUD_HIRES_H
#define HUD_HIRES_H

/* an embedded texture: an 8-bit RGBA PNG, and the bitmap it stands for (its
bitmap group tag's name, its index there, and the CRC-32 of its first mip
level's pixels as the English maps have them); coverage: a meter's, whose
green is how much of each texel its shapes cover (the meter shader reads
only its blue and alpha); title: a menu's title (port/assets/titles, made by
tools/title_assets.py), drawn with display.high_res_text rather than
display.high_res_hud */
struct hud_hires_embedded
{
	const char *tag;
	int bitmap;
	unsigned int width, height;
	unsigned int crc;
	int coverage;
	int title;
	const unsigned int *png;
	unsigned int png_size;
};

extern const struct hud_hires_embedded hud_hires_embedded[];
extern const unsigned int hud_hires_embedded_count;

/* the texture standing for the bitmap whose pixels are uploaded from address
(guest virtual) with this size, its first mip level being level0_size bytes,
or -1: none, its setting (display.high_res_hud, or display.high_res_text for
a title) off, or pixels other than those the
texture was drawn for (another language's maps, which have their own text,
or modified ones) */
long hud_hires_override_find(unsigned long address, unsigned long width, unsigned long height,
	unsigned long level0_size);
/* its GL texture (decoded and uploaded, mipmapped, on first use; 0 if it
could not be), and the number of its mip levels */
unsigned int hud_hires_override_texture(long asset, unsigned long *levels);
/* a GL texture drawn from an 8-bit RGBA PNG (as the tools write them), with
all its mip levels, and their number; 0 if it could not be */
unsigned int hud_hires_png_texture(const void *png, unsigned long size, unsigned long *levels);
/* whether its green is its coverage (d3d8_gl.c, nv2a_psh.c: coverage_alpha) */
int hud_hires_override_coverage(long asset);

#endif
