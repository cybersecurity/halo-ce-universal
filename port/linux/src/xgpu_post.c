/*
XGPU_POST.C

The post-process passes of display.anti_aliasing (xgpu.h): FXAA and SMAA,
run on a render target in place, over the rectangle of a window's 3D view.
Each pass draws one triangle covering the rectangle (its corners from
gl_VertexID, with no vertex data) and finds its texels from gl_FragCoord: a
target's rows are the picture's from the top, as the passes' own textures'
are, so no coordinate is turned over. The target's pixels are copied first,
into a texture of the target's format, so that no pass reads what it writes,
and only colour is written back: the game keeps values of its own in
destination alpha.

FXAA is written here, after Timothy Lottes' description: the direction of an
edge from the luma around a pixel, the edge's ends found by stepping along
it, and the pixel blended across it by where it lies between them, or by how
much it stands out from its neighbours. SMAA is port/third_party/smaa's,
compiled as GLSL at its HIGH preset: edges from luma, blending weights from
its lookup textures, then the blend.
*/

#include "xgpu.h"
#include "memory/zlib/zlib.h"

#include <stdlib.h>

/* port/third_party/smaa, embedded by tools/embed_assets.py: the shader's
text, and the lookup textures' bytes compressed with zlib */
extern const unsigned int xgpu_smaa_shader[];
extern const unsigned int xgpu_smaa_area_texture[];
extern const unsigned long xgpu_smaa_area_texture_size;
extern const unsigned int xgpu_smaa_search_texture[];
extern const unsigned long xgpu_smaa_search_texture_size;

enum
{
	_post_program_fxaa,
	_post_program_smaa_edges,
	_post_program_smaa_weights,
	_post_program_smaa_blend,
	NUMBER_OF_POST_PROGRAMS
};

static struct
{
	GLuint vertex_shader;
	GLuint programs[NUMBER_OF_POST_PROGRAMS];
	/* each program's metrics uniform, and FXAA's bounds */
	GLint metrics[NUMBER_OF_POST_PROGRAMS];
	GLint bounds;
	/* the size of the textures below, and the format of color: the target's */
	unsigned long width, height;
	GLenum format;
	/* the target's pixels before the pass, and SMAA's edges and blending
	weights */
	GLuint color, edges, weights;
	GLuint color_framebuffer, edges_framebuffer, weights_framebuffer;
	/* SMAA's lookup textures */
	GLuint area, search;
	/* linear and clamped, as FXAA and SMAA sample */
	GLuint sampler;
	/* no attributes: the triangle comes from gl_VertexID */
	GLuint vertex_array;
} post;

/* ---------- programs */

static void shader_header(struct xgpu_text *text)
{
#ifdef HALO_ANDROID
	xgpu_text_append(text,
		"#version %s\n"
		"precision highp float;\n"
		"precision highp int;\n"
		"precision highp sampler2D;\n"
		"#define SMAA_GLSL_3 1\n",
		xgpu_capabilities.shading_language);
#else
	xgpu_text_append(text, "#version 450 core\n#define SMAA_GLSL_4 1\n");
#endif
}

/* one triangle over the whole viewport */
static const char vertex_source[] =
	"void main()\n"
	"{\n"
	"\tgl_Position = vec4(float((gl_VertexID & 1) << 2) - 1.0, float((gl_VertexID & 2) << 1) - 1.0, 0.0, 1.0);\n"
	"}\n";

static const char fxaa_source[] =
	"uniform sampler2D color_texture;\n"
	"/* 1 / width, 1 / height, width, height */\n"
	"uniform vec4 metrics;\n"
	"/* the rectangle's outermost texel centres, as texture coordinates */\n"
	"uniform vec4 bounds;\n"
	"out vec4 result;\n"
	"\n"
	"const float EDGE_THRESHOLD = 0.125;\n"
	"const float EDGE_THRESHOLD_MINIMUM = 0.0312;\n"
	"const float SUBPIXEL_QUALITY = 0.75;\n"
	"const int STEPS = 12;\n"
	"const float STEP_LENGTHS[12] = float[12](1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);\n"
	"\n"
	"vec3 color_at(vec2 position)\n"
	"{\n"
	"\treturn textureLod(color_texture, clamp(position, bounds.xy, bounds.zw), 0.0).rgb;\n"
	"}\n"
	"\n"
	"float luma_at(vec2 position)\n"
	"{\n"
	"\treturn dot(color_at(position), vec3(0.299, 0.587, 0.114));\n"
	"}\n"
	"\n"
	"void main()\n"
	"{\n"
	"\tvec2 position = gl_FragCoord.xy * metrics.xy;\n"
	"\tfloat luma = luma_at(position);\n"
	"\tfloat up = luma_at(position + vec2(0.0, -metrics.y));\n"
	"\tfloat down = luma_at(position + vec2(0.0, metrics.y));\n"
	"\tfloat left = luma_at(position + vec2(-metrics.x, 0.0));\n"
	"\tfloat right = luma_at(position + vec2(metrics.x, 0.0));\n"
	"\tfloat highest = max(luma, max(max(up, down), max(left, right)));\n"
	"\tfloat range = highest - min(luma, min(min(up, down), min(left, right)));\n"
	"\n"
	"\t/* no edge: the target has the pixel already */\n"
	"\tif (range < max(EDGE_THRESHOLD_MINIMUM, highest * EDGE_THRESHOLD))\n"
	"\t\tdiscard;\n"
	"\tfloat up_left = luma_at(position + vec2(-metrics.x, -metrics.y));\n"
	"\tfloat up_right = luma_at(position + vec2(metrics.x, -metrics.y));\n"
	"\tfloat down_left = luma_at(position + vec2(-metrics.x, metrics.y));\n"
	"\tfloat down_right = luma_at(position + vec2(metrics.x, metrics.y));\n"
	"\n"
	"\t/* how much the pixel stands out from its neighbourhood (a feature\n"
	"\tthinner than a pixel) */\n"
	"\tfloat average = (2.0 * (up + down + left + right) + up_left + up_right + down_left + down_right) / 12.0;\n"
	"\tfloat subpixel = smoothstep(0.0, 1.0, clamp(abs(average - luma) / range, 0.0, 1.0));\n"
	"\tsubpixel = subpixel * subpixel * SUBPIXEL_QUALITY;\n"
	"\n"
	"\t/* an edge along the rows where the luma changes more from row to row\n"
	"\tthan from column to column; across it, the side it changes more to */\n"
	"\tbool horizontal = abs(up_left + down_left - 2.0 * left) + 2.0 * abs(up + down - 2.0 * luma) +\n"
	"\t\tabs(up_right + down_right - 2.0 * right) >=\n"
	"\t\tabs(up_left + up_right - 2.0 * up) + 2.0 * abs(left + right - 2.0 * luma) +\n"
	"\t\tabs(down_left + down_right - 2.0 * down);\n"
	"\tfloat before = horizontal ? up : left;\n"
	"\tfloat after = horizontal ? down : right;\n"
	"\tbool before_steeper = abs(before - luma) >= abs(after - luma);\n"
	"\tfloat gradient = 0.25 * max(abs(before - luma), abs(after - luma));\n"
	"\tfloat edge_luma = 0.5 * ((before_steeper ? before : after) + luma);\n"
	"\tvec2 across = (horizontal ? vec2(0.0, metrics.y) : vec2(metrics.x, 0.0)) * (before_steeper ? -1.0 : 1.0);\n"
	"\tvec2 along = horizontal ? vec2(metrics.x, 0.0) : vec2(0.0, metrics.y);\n"
	"\n"
	"\t/* along the edge, half a pixel across, to where its luma ends either way */\n"
	"\tvec2 end_before = position + 0.5 * across - along;\n"
	"\tvec2 end_after = position + 0.5 * across + along;\n"
	"\tfloat luma_before = luma_at(end_before) - edge_luma;\n"
	"\tfloat luma_after = luma_at(end_after) - edge_luma;\n"
	"\tbool reached_before = abs(luma_before) >= gradient;\n"
	"\tbool reached_after = abs(luma_after) >= gradient;\n"
	"\tfor (int index = 1; index < STEPS && !(reached_before && reached_after); index++)\n"
	"\t{\n"
	"\t\tif (!reached_before)\n"
	"\t\t{\n"
	"\t\t\tend_before -= along * STEP_LENGTHS[index];\n"
	"\t\t\tluma_before = luma_at(end_before) - edge_luma;\n"
	"\t\t\treached_before = abs(luma_before) >= gradient;\n"
	"\t\t}\n"
	"\t\tif (!reached_after)\n"
	"\t\t{\n"
	"\t\t\tend_after += along * STEP_LENGTHS[index];\n"
	"\t\t\tluma_after = luma_at(end_after) - edge_luma;\n"
	"\t\t\treached_after = abs(luma_after) >= gradient;\n"
	"\t\t}\n"
	"\t}\n"
	"\n"
	"\t/* blended across the edge the more, the nearer the pixel is to an end;\n"
	"\tonly where the luma at that end changes the way the pixel's does */\n"
	"\tfloat distance_before = horizontal ? position.x - end_before.x : position.y - end_before.y;\n"
	"\tfloat distance_after = horizontal ? end_after.x - position.x : end_after.y - position.y;\n"
	"\tfloat pixel_offset = 0.5 - min(distance_before, distance_after) / (distance_before + distance_after);\n"
	"\tbool varies = ((distance_before < distance_after ? luma_before : luma_after) < 0.0) != (luma < edge_luma);\n"
	"\tresult = vec4(color_at(position + max(varies ? pixel_offset : 0.0, subpixel) * across), 1.0);\n"
	"}\n";

/* the main functions of SMAA's passes, after SMAA.hlsl; its vertex
functions' offsets are found for each pixel */
static const char *const smaa_mains[] =
{
	/* edges */
	"uniform sampler2D color_texture;\n"
	"out vec4 result;\n"
	"\n"
	"void main()\n"
	"{\n"
	"\tvec2 texcoord = gl_FragCoord.xy * metrics.xy;\n"
	"\tvec4 offset[3];\n"
	"\tSMAAEdgeDetectionVS(texcoord, offset);\n"
	"\tresult = vec4(SMAALumaEdgeDetectionPS(texcoord, offset, color_texture), 0.0, 0.0);\n"
	"}\n",
	/* weights */
	"uniform sampler2D edges_texture;\n"
	"uniform sampler2D area_texture;\n"
	"uniform sampler2D search_texture;\n"
	"out vec4 result;\n"
	"\n"
	"void main()\n"
	"{\n"
	"\tvec2 texcoord = gl_FragCoord.xy * metrics.xy;\n"
	"\tvec2 pixcoord;\n"
	"\tvec4 offset[3];\n"
	"\tSMAABlendingWeightCalculationVS(texcoord, pixcoord, offset);\n"
	"\tresult = SMAABlendingWeightCalculationPS(texcoord, pixcoord, offset, edges_texture, area_texture,\n"
	"\t\tsearch_texture, vec4(0.0));\n"
	"}\n",
	/* blend */
	"uniform sampler2D color_texture;\n"
	"uniform sampler2D weights_texture;\n"
	"out vec4 result;\n"
	"\n"
	"void main()\n"
	"{\n"
	"\tvec2 texcoord = gl_FragCoord.xy * metrics.xy;\n"
	"\tvec4 offset;\n"
	"\tSMAANeighborhoodBlendingVS(texcoord, offset);\n"
	"\tresult = SMAANeighborhoodBlendingPS(texcoord, offset, color_texture, weights_texture);\n"
	"}\n",
};

/* a program, its samplers given their texture units; 0 if it does not
build */
static GLuint program_build(int which)
{
	static const char *const names[NUMBER_OF_POST_PROGRAMS] =
	{
		"FXAA", "SMAA edge detection", "SMAA blending weight", "SMAA neighborhood blending"
	};
	static const struct
	{
		const char *name;
		GLint unit;
	} units[] =
	{
		{ "color_texture", 0 }, { "edges_texture", 0 }, { "area_texture", 1 },
		{ "search_texture", 2 }, { "weights_texture", 1 },
	};
	struct xgpu_text text = { 0 };
	GLuint fragment_shader, program;
	unsigned long index;

	if (!post.vertex_shader)
	{
		shader_header(&text);
		xgpu_text_append(&text, "%s", vertex_source);
		post.vertex_shader = xgpu_compile_shader(GL_VERTEX_SHADER, text.buffer, "post-process");
		text.length = 0;
	}
	shader_header(&text);
	if (which == _post_program_fxaa)
	{
		xgpu_text_append(&text, "%s", fxaa_source);
	}
	else
	{
		xgpu_text_append(&text,
			"uniform vec4 metrics;\n"
			"#define SMAA_RT_METRICS metrics\n"
			"#define SMAA_PRESET_HIGH 1\n"
			"%s\n%s",
			(const char *)xgpu_smaa_shader, smaa_mains[which - _post_program_smaa_edges]);
	}
	fragment_shader = post.vertex_shader ?
		xgpu_compile_shader(GL_FRAGMENT_SHADER, text.buffer, names[which]) :
		0;
	free(text.buffer);
	if (!fragment_shader)
		return 0;
	program = xgpu_link_program(post.vertex_shader, fragment_shader, names[which]);
	glDeleteShader(fragment_shader);
	if (!program)
		return 0;
	glUseProgram(program);
	/* (a name the program does not have is location -1, which is ignored) */
	for (index = 0; index < sizeof(units) / sizeof(units[0]); index++)
		glUniform1i(glGetUniformLocation(program, units[index].name), units[index].unit);
	post.metrics[which] = glGetUniformLocation(program, "metrics");
	if (which == _post_program_fxaa)
		post.bounds = glGetUniformLocation(program, "bounds");
	return program;
}

/* ---------- textures */

static GLuint texture_new(GLenum format, unsigned long width, unsigned long height, GLenum data_format,
	const void *data)
{
	GLuint texture;

	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	glTexImage2D(GL_TEXTURE_2D, 0, (GLint)format, (GLsizei)width, (GLsizei)height, 0, data_format,
		GL_UNSIGNED_BYTE, data);
	return texture;
}

/* a texture made as a render target's color texture of this format, for a
copy of the target's pixels */
static GLuint target_texture_new(GLenum format, unsigned long width, unsigned long height)
{
	GLuint texture;

	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	xgpu_render_target_image(format, width, height);
	return texture;
}

static GLuint framebuffer_new(GLuint texture)
{
	GLuint framebuffer;
	GLenum draw_buffer = GL_COLOR_ATTACHMENT0;

	glGenFramebuffers(1, &framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
	glDrawBuffers(1, &draw_buffer);
	return framebuffer;
}

/* the passes' textures, of the target's size, and color of its format (SMAA's
only for SMAA) */
static void textures_fit(BOOL smaa, GLenum format, unsigned long width, unsigned long height)
{
	if (post.width != width || post.height != height || post.format != format)
	{
		GLuint textures[3] = { post.color, post.edges, post.weights };
		GLuint framebuffers[3] = { post.color_framebuffer, post.edges_framebuffer, post.weights_framebuffer };

		glDeleteFramebuffers(3, framebuffers);
		glDeleteTextures(3, textures);
		post.color = post.edges = post.weights = 0;
		post.width = width;
		post.height = height;
		post.format = format;
		post.color = target_texture_new(format, width, height);
		post.color_framebuffer = framebuffer_new(post.color);
	}
	if (smaa && !post.edges)
	{
		post.edges = texture_new(GL_RG8, width, height, GL_RG, NULL);
		post.weights = texture_new(GL_RGBA8, width, height, GL_RGBA, NULL);
		post.edges_framebuffer = framebuffer_new(post.edges);
		post.weights_framebuffer = framebuffer_new(post.weights);
	}
}

/* a lookup texture of SMAA's from its zlib stream; 0 if it does not
inflate */
static GLuint lookup_texture(const unsigned int *stream, unsigned long stream_size, GLenum format,
	GLenum data_format, unsigned long width, unsigned long height, unsigned long texel_size)
{
	uLongf size = (uLongf)(width * height * texel_size);
	unsigned char *texels = malloc(size);
	GLuint texture = 0;
	int result;

	if (!texels)
		return 0;
	result = uncompress(texels, &size, (const Bytef *)stream, (uLong)stream_size);
	/* (the game's zlib is 1.1, which can stop short of saying the stream
	has ended: the size says whether it is whole) */
	if ((result == Z_OK || result == Z_BUF_ERROR) && size == (uLongf)(width * height * texel_size))
	{
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		texture = texture_new(format, width, height, data_format, texels);
	}
	free(texels);
	return texture;
}

/* ---------- the passes */

static void texture_bind(GLuint unit, GLuint texture)
{
	glActiveTexture(GL_TEXTURE0 + unit);
	glBindTexture(GL_TEXTURE_2D, texture);
	glBindSampler(unit, post.sampler);
}

/* a program's triangle drawn into a framebuffer over the rectangle;
color_only keeps the framebuffer's alpha */
static void pass_draw(int which, GLuint framebuffer, const GLint corners[4], BOOL color_only)
{
	float metrics[4];

	metrics[0] = 1.0f / (float)post.width;
	metrics[1] = 1.0f / (float)post.height;
	metrics[2] = (float)post.width;
	metrics[3] = (float)post.height;
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glViewport(corners[0], corners[1], corners[2] - corners[0], corners[3] - corners[1]);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, color_only ? GL_FALSE : GL_TRUE);
	glUseProgram(post.programs[which]);
	glUniform4fv(post.metrics[which], 1, metrics);
	glDrawArrays(GL_TRIANGLES, 0, 3);
}

/* an intermediate emptied, all of it, so that no edge of another window's
pass is found: SMAA's passes write only where there is an edge */
static void intermediate_clear(GLuint framebuffer)
{
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClear(GL_COLOR_BUFFER_BIT);
}

BOOL xgpu_post_anti_alias(BOOL smaa, GLuint framebuffer, GLenum format, unsigned long width,
	unsigned long height, const GLint corners[4])
{
	int first = smaa ? _post_program_smaa_edges : _post_program_fxaa;
	int last = smaa ? _post_program_smaa_blend : _post_program_fxaa;
	int which;

	/* (the first time: the caller does not ask again once this fails) */
	for (which = first; which <= last; which++)
	{
		if (!post.programs[which] && !(post.programs[which] = program_build(which)))
			return FALSE;
	}
	if (smaa && !post.area)
	{
		post.area = lookup_texture(xgpu_smaa_area_texture, xgpu_smaa_area_texture_size, GL_RG8, GL_RG,
			160, 560, 2);
		post.search = lookup_texture(xgpu_smaa_search_texture, xgpu_smaa_search_texture_size, GL_R8, GL_RED,
			64, 16, 1);
		if (!post.area || !post.search)
		{
			platform_log("cannot inflate SMAA's lookup textures");
			return FALSE;
		}
	}
	if (!post.sampler)
	{
		glGenSamplers(1, &post.sampler);
		glSamplerParameteri(post.sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glSamplerParameteri(post.sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glSamplerParameteri(post.sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glSamplerParameteri(post.sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glGenVertexArrays(1, &post.vertex_array);
	}
	textures_fit(smaa, format, width, height);

	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_CULL_FACE);
	glDisable(GL_POLYGON_OFFSET_FILL);
#ifndef HALO_ANDROID
	glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
#endif
	glBindVertexArray(post.vertex_array);

	/* the target's pixels, read while the target is drawn into */
	glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, post.color_framebuffer);
	glBlitFramebuffer(corners[0], corners[1], corners[2], corners[3], corners[0], corners[1], corners[2], corners[3],
		GL_COLOR_BUFFER_BIT, GL_NEAREST);

	if (!smaa)
	{
		float bounds[4];

		/* (split screen: no texel of the next window is read) */
		bounds[0] = ((float)corners[0] + 0.5f) / (float)width;
		bounds[1] = ((float)corners[1] + 0.5f) / (float)height;
		bounds[2] = ((float)corners[2] - 0.5f) / (float)width;
		bounds[3] = ((float)corners[3] - 0.5f) / (float)height;
		texture_bind(0, post.color);
		glUseProgram(post.programs[_post_program_fxaa]);
		glUniform4fv(post.bounds, 1, bounds);
		pass_draw(_post_program_fxaa, framebuffer, corners, TRUE);
	}
	else
	{
		intermediate_clear(post.edges_framebuffer);
		texture_bind(0, post.color);
		pass_draw(_post_program_smaa_edges, post.edges_framebuffer, corners, FALSE);

		intermediate_clear(post.weights_framebuffer);
		texture_bind(0, post.edges);
		texture_bind(1, post.area);
		texture_bind(2, post.search);
		pass_draw(_post_program_smaa_weights, post.weights_framebuffer, corners, FALSE);

		texture_bind(0, post.color);
		texture_bind(1, post.weights);
		pass_draw(_post_program_smaa_blend, framebuffer, corners, TRUE);
	}
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	return TRUE;
}
