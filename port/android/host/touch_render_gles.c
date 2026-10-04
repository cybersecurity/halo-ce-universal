/*
TOUCH_RENDER_GLES.C

Draws the touch controls over the game with OpenGL ES 3.

TC_Render runs in host_sdl_gl_swap_window, on the guest's render thread, with
the game's context current. The game's renderer (port/linux/src/d3d8_gl.c)
keeps its own idea of the GL state, so everything touched here is saved first
and put back after: the program, the vertex array, the array buffer, the
framebuffer, the viewport, the blend, depth, cull, scissor and stencil
settings, and the write masks.
*/

#include "host.h"
#include "touch_controls.h"

#include <GLES3/gl32.h>
#include <string.h>

#define SEGMENTS 32
#define MAX_VERTICES 16384

struct vertex
{
	float x, y, r, g, b, a;
};

static struct vertex vertices[MAX_VERTICES];
static int vertex_count;

static GLuint program, vao, vbo;
static GLint resolution_location;
static bool gl_failed;

/* ---------- tiny 5x7 font for the button labels */

static const struct
{
	char letter;
	unsigned char rows[7];	/* bit 4 is the left column */
} font[] =
{
	{ 'A', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
	{ 'B', { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E } },
	{ 'C', { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E } },
	{ 'F', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 } },
	{ 'G', { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E } },
	{ 'I', { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E } },
	{ 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F } },
	{ 'X', { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 } },
	{ 'Y', { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 } },
	{ 'Z', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F } },
	{ 'E', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F } },
	{ 'K', { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 } },
	{ 'O', { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
	{ 'S', { 0x0E, 0x11, 0x10, 0x0E, 0x01, 0x11, 0x0E } },
	{ 'R', { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 } },
	{ '+', { 0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00 } },
	{ '-', { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 } },
};

/* ---------- geometry into the vertex array */

static void push(float x, float y, const float *color)
{
	struct vertex *vertex;

	if (vertex_count >= MAX_VERTICES)
		return;
	vertex = &vertices[vertex_count++];
	vertex->x = x;
	vertex->y = y;
	vertex->r = color[0];
	vertex->g = color[1];
	vertex->b = color[2];
	vertex->a = color[3];
}

static void quad(float x0, float y0, float x1, float y1, const float *color)
{
	push(x0, y0, color);
	push(x1, y0, color);
	push(x1, y1, color);
	push(x0, y0, color);
	push(x1, y1, color);
	push(x0, y1, color);
}

static void disc(float cx, float cy, float radius, const float *color)
{
	int index;

	for (index = 0; index < SEGMENTS; index++)
	{
		float a0 = 2.0f * SDL_PI_F * (float)index / (float)SEGMENTS;
		float a1 = 2.0f * SDL_PI_F * (float)(index + 1) / (float)SEGMENTS;

		push(cx, cy, color);
		push(cx + SDL_cosf(a0) * radius, cy + SDL_sinf(a0) * radius, color);
		push(cx + SDL_cosf(a1) * radius, cy + SDL_sinf(a1) * radius, color);
	}
}

static void ring(float cx, float cy, float radius, float thickness, const float *color)
{
	float inner = radius - thickness;
	int index;

	for (index = 0; index < SEGMENTS; index++)
	{
		float a0 = 2.0f * SDL_PI_F * (float)index / (float)SEGMENTS;
		float a1 = 2.0f * SDL_PI_F * (float)(index + 1) / (float)SEGMENTS;
		float c0 = SDL_cosf(a0), s0 = SDL_sinf(a0), c1 = SDL_cosf(a1), s1 = SDL_sinf(a1);

		push(cx + c0 * radius, cy + s0 * radius, color);
		push(cx + c0 * inner, cy + s0 * inner, color);
		push(cx + c1 * radius, cy + s1 * radius, color);
		push(cx + c0 * inner, cy + s0 * inner, color);
		push(cx + c1 * inner, cy + s1 * inner, color);
		push(cx + c1 * radius, cy + s1 * radius, color);
	}
}

static void label(const char *text, float cx, float cy, float radius, const float *color)
{
	float pixel = SDL_max(2.0f, SDL_floorf(radius / 9.0f));
	float advance = 6.0f * pixel;
	float width = (float)SDL_strlen(text) * advance - pixel;
	float x = cx - width * 0.5f;
	float y = cy - 3.5f * pixel;
	size_t glyph;
	int row, column;

	for (; *text; text++, x += advance)
	{
		for (glyph = 0; glyph < SDL_arraysize(font); glyph++)
		{
			if (font[glyph].letter == *text)
				break;
		}
		if (glyph == SDL_arraysize(font))
			continue;
		for (row = 0; row < 7; row++)
		{
			for (column = 0; column < 5; column++)
			{
				if (font[glyph].rows[row] & (0x10 >> column))
				{
					float px = x + (float)column * pixel, py = y + (float)row * pixel;

					quad(px, py, px + pixel, py + pixel, color);
				}
			}
		}
	}
}

/* ---------- GL objects */

static GLuint compile(GLenum type, const char *source)
{
	GLuint shader = glCreateShader(type);
	GLint ok = 0;

	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok)
	{
		char log[256];

		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		host_logf(HOST_LOG_ERROR, "touch: shader: %s", log);
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

static bool create_objects(void)
{
	static const char *vertex_source =
		"#version 300 es\n"
		"layout(location = 0) in vec2 a_position;\n"
		"layout(location = 1) in vec4 a_color;\n"
		"uniform vec2 u_resolution;\n"
		"out vec4 v_color;\n"
		"void main()\n"
		"{\n"
		"\tvec2 p = a_position / u_resolution * 2.0 - 1.0;\n"
		"\tgl_Position = vec4(p.x, -p.y, 0.0, 1.0);\n"
		"\tv_color = a_color;\n"
		"}\n";
	static const char *fragment_source =
		"#version 300 es\n"
		"precision mediump float;\n"
		"in vec4 v_color;\n"
		"out vec4 o_color;\n"
		"void main() { o_color = v_color; }\n";
	GLuint vs, fs;
	GLint ok = 0;

	vs = compile(GL_VERTEX_SHADER, vertex_source);
	fs = compile(GL_FRAGMENT_SHADER, fragment_source);
	if (!vs || !fs)
		return false;
	program = glCreateProgram();
	glAttachShader(program, vs);
	glAttachShader(program, fs);
	glLinkProgram(program);
	glDeleteShader(vs);
	glDeleteShader(fs);
	glGetProgramiv(program, GL_LINK_STATUS, &ok);
	if (!ok)
	{
		host_logf(HOST_LOG_ERROR, "touch: the HUD program does not link");
		return false;
	}
	resolution_location = glGetUniformLocation(program, "u_resolution");
	glGenVertexArrays(1, &vao);
	glGenBuffers(1, &vbo);
	return true;
}

/* ---------- the frame */

static void build_vertices(const TC_Context *c)
{
	const float opacity = c->opacity;
	int index;

	vertex_count = 0;

	/* the editor dims the game so the controls stand out */
	if (c->edit)
	{
		float dim[4] = { 0.0f, 0.0f, 0.0f, 0.40f };

		quad(0.0f, 0.0f, (float)c->view_w, (float)c->view_h, dim);
	}

	for (index = 0; index < c->widget_count; index++)
	{
		const TC_Widget *widget = &c->widgets[index];
		float fill[4] = { 1.0f, 1.0f, 1.0f, 0.0f };
		float edge[4] = { 1.0f, 1.0f, 1.0f, 0.50f * opacity };
		float text[4] = { 1.0f, 1.0f, 1.0f, 0.85f * opacity };
		float thickness = 3.0f;

		if (!TC_WidgetShown(c, widget))
			continue;

		if (widget->type == TC_WIDGET_STICK)
		{
			float knob[4] = { 1.0f, 1.0f, 1.0f, (widget->active ? 0.55f : 0.30f) * opacity };

			fill[3] = (widget->active ? 0.22f : 0.12f) * opacity;
			edge[3] = (c->edit ? 0.90f : 0.45f) * opacity;
			thickness = c->edit ? 6.0f : 3.0f;
			disc(widget->cx, widget->cy, widget->radius, fill);
			ring(widget->cx, widget->cy, widget->radius, thickness, edge);
			disc(widget->cx + widget->knob_x, widget->cy + widget->knob_y, widget->radius * 0.42f, knob);
		}
		else if (widget->type == TC_WIDGET_UI)
		{
			/* the "E" fills up while it is held; the editor's buttons are solid */
			float progress = widget->hold / 0.8f;

			if (widget->ui == TC_UI_EDIT)
				fill[3] = (0.14f + 0.55f * SDL_clamp(progress, 0.0f, 1.0f)) * opacity;
			else
				fill[3] = 0.55f;
			edge[3] = c->edit ? 0.95f : 0.40f * opacity;
			if (c->edit)
				text[3] = 1.0f;
			disc(widget->cx, widget->cy, widget->radius, fill);
			ring(widget->cx, widget->cy, widget->radius, 3.0f, edge);
			label(widget->label, widget->cx, widget->cy, widget->radius, text);
		}
		else
		{
			fill[3] = (widget->active ? 0.55f : 0.20f) * opacity;
			if (c->edit)
			{
				/* bright outline: these can be dragged */
				edge[3] = 0.95f;
				fill[3] = widget->active ? 0.65f : 0.30f;
				thickness = 6.0f;
				text[3] = 1.0f;
			}
			disc(widget->cx, widget->cy, widget->radius, fill);
			ring(widget->cx, widget->cy, widget->radius, thickness, edge);
			label(widget->label, widget->cx, widget->cy, widget->radius, text);
		}
	}
}

void TC_Render(TC_Context *c)
{
	GLint old_program, old_vao, old_array_buffer, old_framebuffer, old_viewport[4];
	GLint old_blend_src_rgb, old_blend_dst_rgb, old_blend_src_alpha, old_blend_dst_alpha;
	GLint old_blend_equation_rgb, old_blend_equation_alpha;
	GLboolean old_blend, old_depth, old_cull, old_scissor, old_stencil, old_depth_mask, old_color_mask[4];

	if (!c->lock || gl_failed)
		return;
	SDL_LockMutex(c->lock);
	if (!c->visible || !c->backend_on || c->view_w <= 0 || c->view_h <= 0)
	{
		SDL_UnlockMutex(c->lock);
		return;
	}

	if (!program && !create_objects())
	{
		gl_failed = true;	/* the HUD is lost, the game is not */
		SDL_UnlockMutex(c->lock);
		return;
	}
	build_vertices(c);

	glGetIntegerv(GL_CURRENT_PROGRAM, &old_program);
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &old_vao);
	glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &old_array_buffer);
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_framebuffer);
	glGetIntegerv(GL_VIEWPORT, old_viewport);
	glGetIntegerv(GL_BLEND_SRC_RGB, &old_blend_src_rgb);
	glGetIntegerv(GL_BLEND_DST_RGB, &old_blend_dst_rgb);
	glGetIntegerv(GL_BLEND_SRC_ALPHA, &old_blend_src_alpha);
	glGetIntegerv(GL_BLEND_DST_ALPHA, &old_blend_dst_alpha);
	glGetIntegerv(GL_BLEND_EQUATION_RGB, &old_blend_equation_rgb);
	glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &old_blend_equation_alpha);
	old_blend = glIsEnabled(GL_BLEND);
	old_depth = glIsEnabled(GL_DEPTH_TEST);
	old_cull = glIsEnabled(GL_CULL_FACE);
	old_scissor = glIsEnabled(GL_SCISSOR_TEST);
	old_stencil = glIsEnabled(GL_STENCIL_TEST);
	glGetBooleanv(GL_DEPTH_WRITEMASK, &old_depth_mask);
	glGetBooleanv(GL_COLOR_WRITEMASK, old_color_mask);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	glViewport(0, 0, c->view_w, c->view_h);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_STENCIL_TEST);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glEnable(GL_BLEND);
	glBlendEquation(GL_FUNC_ADD);
	glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

	glUseProgram(program);
	glUniform2f(resolution_location, (float)c->view_w, (float)c->view_h);
	glBindVertexArray(vao);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)vertex_count * sizeof(struct vertex)), vertices, GL_STREAM_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(struct vertex), (const void *)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(struct vertex), (const void *)(2 * sizeof(float)));
	glDrawArrays(GL_TRIANGLES, 0, vertex_count);

	glBindVertexArray((GLuint)old_vao);
	glBindBuffer(GL_ARRAY_BUFFER, (GLuint)old_array_buffer);
	glUseProgram((GLuint)old_program);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)old_framebuffer);
	glViewport(old_viewport[0], old_viewport[1], old_viewport[2], old_viewport[3]);
	glBlendEquationSeparate((GLenum)old_blend_equation_rgb, (GLenum)old_blend_equation_alpha);
	glBlendFuncSeparate((GLenum)old_blend_src_rgb, (GLenum)old_blend_dst_rgb,
		(GLenum)old_blend_src_alpha, (GLenum)old_blend_dst_alpha);
	glColorMask(old_color_mask[0], old_color_mask[1], old_color_mask[2], old_color_mask[3]);
	glDepthMask(old_depth_mask);
	if (old_blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
	if (old_depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
	if (old_cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
	if (old_scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
	if (old_stencil) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);

	SDL_UnlockMutex(c->lock);
}
