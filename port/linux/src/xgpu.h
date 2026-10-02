/*
XGPU.H

Internals shared by the OpenGL implementation of the Xbox Direct3D API:
the NV2A shader translators (nv2a_vsh.c, nv2a_psh.c), texture decoding
(xbox_textures.c), guest memory write tracking (memory_watch.c) and the
device itself (d3d8_gl.c).
*/

#ifndef __HALO_LINUX_XGPU_H
#define __HALO_LINUX_XGPU_H

#include "platform.h"
#include "gl.h"

#ifdef HALO_ANDROID
/* OpenGL ES features that are optional (d3d8_gl.c gl_initialize) */
struct xgpu_capabilities
{
	BOOL copy_image;
	BOOL border_clamp;
	BOOL anisotropy;
	BOOL s3tc;
	/* ES 3.2: glDrawElementsBaseVertex */
	BOOL base_vertex;
	/* ES 3.1 with fragment atomic counters: exact visibility test counts */
	BOOL atomic_counters;
	/* "300 es" or "310 es" */
	const char *shading_language;
};

extern struct xgpu_capabilities xgpu_capabilities;

/* port/android/guest/runtime/guest_host.h */
int host_gl_has_extension(const char *name);
unsigned int host_gl_read_buffer_word(unsigned int buffer, unsigned int offset);
void host_gl_buffer_write(unsigned int target, unsigned int offset, unsigned int size, const void *data);
void host_gl_fence_frame(unsigned int slot);
void host_gl_wait_frame(unsigned int slot);
#endif

/* ---------- GL state

The device caches the GL state it sets for draws (d3d8_gl.c); code that
changes GL state behind it (binding a texture to upload it, deleting one)
must call this afterwards. */

void xgpu_gl_state_invalidate(void);

/* a compiled shader, or a linked program of two, or 0 with the log
written */
GLuint xgpu_compile_shader(GLenum type, const char *source, const char *what);
GLuint xgpu_link_program(GLuint vertex_shader, GLuint fragment_shader, const char *what);

/* ---------- generated source text */

struct xgpu_text
{
	char *buffer;
	unsigned long length;
	unsigned long capacity;
};

void xgpu_text_append(struct xgpu_text *text, const char *format, ...) __attribute__((format(printf, 2, 3)));

/* ---------- vertex shaders */

#define XGPU_VERTEX_ATTRIBUTE_COUNT 16
#define XGPU_VERTEX_CONSTANT_COUNT 192
/* D3D constant register -96 is hardware register 0 */
#define XGPU_VERTEX_CONSTANT_BIAS 96

/* GLSL for an NV2A vertex program (the instruction words after the program
header). Attributes whose bit is set in packed_attribute_mask are fed as
NORMPACKED3 32-bit integers and unpacked in the shader. Returns a malloc'd
string. */
char *nv2a_vertex_shader_to_glsl(const DWORD *instructions, unsigned long instruction_count,
	unsigned long packed_attribute_mask);

/* ---------- pixel shaders */

enum
{
	_xgpu_sampler_none = 0,
	_xgpu_sampler_2d,
	_xgpu_sampler_3d,
	_xgpu_sampler_cube,
};

/* everything a translated pixel shader depends on; the GLSL program cache
is keyed by these bytes */
struct nv2a_pixel_shader_key
{
	DWORD combiner_state[D3DRS_PS_MAX];
	/* D3DRS_PSTEXTUREMODES lies past D3DRS_PS_MAX */
	DWORD texture_modes;
	unsigned char sampler_type[4];
	unsigned char alpha_kill[4];
	/* D3DTSS_COLORSIGN: channels (bit 0 alpha ... bit 3 blue, as
	D3DTSIGN_*) that hold signed data in an unsigned texture format */
	unsigned char color_sign[4];
	/* D3DCMP_* function for the alpha test, or 0 when disabled */
	unsigned long alpha_test_function;
	unsigned char fog_enable;
	unsigned char fog_table_mode;
	/* inside a visibility test: count the samples that pass (Android) */
	unsigned char count_samples;
	/* a high-res HUD meter (hud_hires.h) drawn with the meter's blend (the
	destination kept by the source's alpha): that alpha is eased to 1 by the
	coverage texture 0's green holds, so that the meter darkens what is
	behind it only where it covers it (the Xbox's point-sampled meters stop
	at their texels' edges; filtered ones have a fringe of faint texels) */
	unsigned char coverage_alpha;
	/* drawn into a multisampled target (display.anti_aliasing "msaa"), its
	sample count: the alpha test covers samples in proportion to how far
	alpha is past the reference, not all of the pixel or none of it, so that
	cut-out edges (foliage, grates) are antialiased too */
	unsigned char alpha_test_samples;
};

char *nv2a_pixel_shader_to_glsl(const struct nv2a_pixel_shader_key *key);

#ifdef HALO_ANDROID
/* ES samplers have no LOD bias of their own */
#define XGPU_PIXEL_UNIFORMS_ES "uniform vec4 texture_lod_bias;\n"
#else
#define XGPU_PIXEL_UNIFORMS_ES ""
#endif

/* the combiner registers that live in uniforms rather than in the program:
C0/C1 of each stage and the final combiner, and texture constants */
#define XGPU_PIXEL_UNIFORMS \
	"uniform vec4 ps_c0[8];\n" \
	"uniform vec4 ps_c1[8];\n" \
	"uniform vec4 ps_final_c0;\n" \
	"uniform vec4 ps_final_c1;\n" \
	"uniform vec4 fog_color;\n" \
	"uniform vec4 fog_parameters;\n" \
	"uniform float alpha_reference;\n" \
	"uniform vec4 bump_matrix[4];\n" \
	"uniform vec4 bump_luminance[4];\n" \
	"uniform vec4 texture_scale[4];\n" \
	XGPU_PIXEL_UNIFORMS_ES

/* ---------- textures */

struct xgpu_texture_description
{
	DWORD format;       /* D3DFMT_* */
	unsigned long width, height, depth, levels;
	BOOL cube_map;
	BOOL linear;        /* not swizzled; addressed with texel coordinates */
	BOOL compressed;
	unsigned long pitch; /* linear textures */
	BOOL hires;         /* a high-res HUD texture drawn in the texture's place (hud_hires.h) */
	BOOL hires_coverage; /* ... whose green is its coverage (a meter's) */
};

void xgpu_texture_describe(DWORD format_word, DWORD size_word, struct xgpu_texture_description *description);
/* bytes of one face, mip levels included (cube faces are padded) */
unsigned long xgpu_texture_face_size(const struct xgpu_texture_description *description);
unsigned long xgpu_texture_level_offset(const struct xgpu_texture_description *description, unsigned long level);
unsigned long xgpu_texture_level_pitch(const struct xgpu_texture_description *description, unsigned long level);

/* the GL texture for an Xbox texture header, uploading or refreshing it
from guest memory as needed; *target receives GL_TEXTURE_2D etc. */
GLuint xgpu_texture_get(const DWORD *resource, const D3DCOLOR *palette, GLenum *target,
	struct xgpu_texture_description *description);
void xgpu_texture_cache_begin_frame(void);

/* ---------- render targets */

struct xgpu_render_target
{
	unsigned long data;  /* physical address */
	unsigned long width, height;
	BOOL depth;
	GLuint texture;
	/* pixels per unit of width and height: more than 1 for the screen's
	targets when the game draws at the display's resolution (d3d8_gl.c) */
	float scale[2];
	unsigned long gl_width, gl_height;
	/* the texture's GL format, which its multisampled renderbuffer and the
	anti-aliasing passes' copy of its pixels (xgpu_post.c) are made with too:
	the blits between them copy the pixels exactly only between the same
	formats */
	GLenum format;
	/* with multisampling, the screen's targets' multisampled renderbuffer,
	which draws go to, and whether it has been drawn into since the texture
	last had its pixels (render_target_resolve) */
	GLuint multisample;
	BOOL unresolved;
};

/* the GL texture holding a render target with this physical address, or 0 */
struct xgpu_render_target *xgpu_render_target_find(unsigned long data);

/* level 0 of the bound 2D texture, width by height, made as a render target's
color texture of this format */
void xgpu_render_target_image(GLenum format, unsigned long width, unsigned long height);

/* ---------- anti-aliasing

display.anti_aliasing (d3d8_gl.c): FXAA or SMAA antialias each window's 3D
view in place before the HUD and menus are drawn over it, so that their
text stays sharp; supersampling draws the screen's targets at twice the
resolution, scaled down as the frame is shown, and multisampling draws them
with several samples a pixel. */

/* FXAA, or SMAA, on the corners x0, y0 to x1, y1 (from row 0) of a render
target's framebuffer, width by height, its color of this format; FALSE if its
programs do not build */
BOOL xgpu_post_anti_alias(BOOL smaa, GLuint framebuffer, GLenum format, unsigned long width,
	unsigned long height, const GLint corners[4]);

#endif
