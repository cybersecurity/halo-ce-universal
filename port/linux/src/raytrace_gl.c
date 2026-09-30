/*
RAYTRACE_GL.C

Ray-traced lighting for the native ports (display.ray_tracing): rays
through the screen's depth on every port, and on macOS also rays through
the level itself with Metal (port/macos/host/host_metal_rt.m).

Once the game has drawn a window's opaque world, before its transparent
geometry, fog, effects and HUD (source/render/render.c), rays are marched
through that window's depth buffer, the scene's geometry as the camera
sees it:

- ambient occlusion: from each pixel, rays across the hemisphere around the
  surface's normal (from the depth's neighbours); what they hit close by
  darkens the pixel, as creases, corners and the ground under objects are
  in the real world;
- one bounce of indirect light: the colour a ray hits lights the pixel a
  little, so a red wall tints the floor beside it;
- reflections: a ray along the view's mirror direction; where it hits, the
  surface reflects that colour, by the Fresnel term (glancing angles
  reflect most), so floors and wet ground pick up the scene.

Rays through the screen's depth find only what the camera sees: rays
leaving the screen find nothing, and are faded out. On macOS, where Metal
can trace rays (in compute on M1 and M2, in the ray tracing hardware of the
M3 and later), the occlusion and reflections are also traced through the
level's own geometry (its collision surfaces,
port/linux/game/raytrace_world.c), which finds what the camera does not
see; the screen's rays still find the objects, which the level does not
hold. The pass costs a few milliseconds at the display's resolution.

It runs as two draws: rays into an effect texture (occlusion, reflection),
then the composite back into the window, with a depth-aware blur of the
occlusion. F9 switches it on and off while playing.
*/

#include "platform.h"
#include "gl.h"
#include "port_config.h"
#include "raytrace_gl.h"
#ifdef HALO_MACOS
/* the host's Metal ray tracing (port/macos/host/host_metal_rt.m) */
#include "guest_host_desktop.h"
#endif

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* port/linux/game/raytrace_world.c: the level's triangles */
unsigned long halo_ray_tracing_world(const float **vertices, long *vertex_count, const unsigned long **indices,
	long *triangle_count);
/* the direction towards the sky's sun; 0 if none */
unsigned char halo_ray_tracing_sun(float *direction);
/* the emitters (port/linux/game/raytrace_world.c): 8 floats each */
long halo_ray_tracing_emitters(float *emitters, long maximum, const float *camera);
/* the dynamic lights (source/objects/object_lights.c): 12 floats each */
long halo_ray_tracing_lights(float *lights, long maximum, long all);
/* the objects as shapes for the rays (port/linux/game/raytrace_world.c) */
long halo_ray_tracing_objects(float *triangles, unsigned char *groups, float *cutouts, long maximum,
	const float *camera,
	float *player_sphere, long shapes);
/* the drawn level, its materials and lightmap pages, and the sky's light
(port/linux/game/raytrace_world.c) */
unsigned long halo_ray_tracing_level(const float **vertices, const float **texcoords, const float **base_texcoords,
	long *vertex_count, const unsigned long **indices, const unsigned long **triangle_materials, long *triangle_count,
	long *cutout_start);
/* the cutouts' masks, decoded as the texture cache loads their bitmaps */
unsigned char halo_ray_tracing_mask(long *index, const unsigned char **alpha, long *width, long *height,
	unsigned long *generation);
unsigned char halo_ray_tracing_level_materials(const float **materials, long *count);
unsigned char halo_ray_tracing_level_page(long *page, const unsigned char **pixels, long *width, long *height);
unsigned char halo_ray_tracing_sky(float *sky);
void halo_ray_tracing_level_pages(long *done, long *total);

/* d3d8_gl.c: the window's current targets and viewport, in GL pixels */
int xgpu_current_targets(GLuint *color, GLuint *depth, int *width, int *height, int viewport[4]);
void xgpu_gl_state_invalidate(void);
void xgpu_gl_bind_device_vertex_array(void);

enum
{
	_ray_tracing_off,
	_ray_tracing_on,
	_ray_tracing_debug_occlusion,
	_ray_tracing_debug_depth,
	/* what Metal's rays find, from the camera: all the screen, its right half */
	_ray_tracing_debug_rays,
	_ray_tracing_debug_split,
};

static struct
{
	int initialized;
	int failed;
	int mode;
	int enabled;
	float occlusion_strength, reflection_strength, bounce_strength, shadow_strength, radius;
	int objects;
	/* the objects' shapes in the rays: 0 the drawn models, 1 the collision
	models */
	int shapes;
	/* display.ray_tracing_lights: every light traced, in place of the game's */
	int traced_lights;
	/* display.ray_tracing_level: the drawn level in the rays (else its
	collision surfaces); display.ray_tracing_gi: the traced light in place of
	the lightmaps' (1; 2 without the lightmaps' light where the rays land),
	its parts' strengths, and the light buffer's pass */
	int drawn_level, gi;
	float gi_sun, gi_bounce, gi_glow, gi_lights;
	/* the path tracer's bounces at most, the traced light's rays a pixel */
	float gi_bounces, gi_samples;
	/* the lightmap pages' average light (sums of a sample of their pixels,
	and how many), for where a page is missing */
	double lightmap_sum[3], lightmap_samples;
	unsigned long level_generation;
	GLuint inject_program;
	GLint inject_uniforms, inject_depth, inject_irradiance, inject_gbuffer, inject_split, inject_results, inject_fallback;
	/* the lightmaps' average light (the inject's where it has none) */
	float lightmap_average[3];
	GLint inject_objects, inject_cameras, inject_grid;
	/* the last frame's rays, for the light buffer: their textures, the
	trace grid (origin, size), the camera's tan and aspect; whether there
	are some */
	GLuint gi_results_texture, gi_lights_texture, gi_gbuffer_texture;
	/* the traced light, denoised on the rays' grid (rgb, a: whether there is
	some), and its pass */
	GLuint denoised_texture, denoise_program;
	/* (the denoise passes in between) */
	GLuint denoising_texture;
	/* what the light buffer took this frame (the window's resolution; 0
	where it took the game's), and whether it did */
	GLuint applied_texture;
	int gi_applied;
	GLint composite_denoised, composite_applied, composite_objects, composite_gbuffer, composite_correct;
	GLint denoise_uniforms, denoise_lights, denoise_results, denoise_gbuffer, denoise_counts;
	float gi_grid[4], gi_tan, gi_aspect;
	int gi_previous;
	int gi_split;
	GLuint irradiance_texture, gbuffer_texture;
	/* the last frame's camera (13 values); whether this frame's rays were
	traced for the light buffer already, and their results */
	float previous_camera[13];
	int gi_traced;
	GLuint gi_traced_results;
	GLuint trace_program, composite_program;
	GLint trace_uniforms, composite_uniforms;
	GLint trace_scene, trace_depth, composite_scene, composite_depth, composite_effect;
	GLint composite_debug, composite_rt, composite_rt_enabled;
	GLint composite_baked, composite_lit, composite_light_split, composite_lit_rt;
	/* the traced share of the dynamic lights' light (Metal's third texture) */
	GLuint lights_texture;
	/* the light before and after the dynamic lights (half resolution), and
	the stages taken this frame (bits 0 and 1) */
	GLuint baked_texture, lit_texture, light_framebuffer;
	int light_stages;
	/* the objects' depth, before the level is drawn (half resolution,
	packed in RGBA8), for their pixels */
	GLuint objects_texture, objects_program;
	GLint objects_uniforms, objects_depth, gbuffer_objects, gbuffer_objects_known;
	/* the world-space rays (macOS: Metal), and whether their programs were
	made (and tried: "screen" makes them only when Metal's rays are asked
	for) */
	int hardware;
	int hardware_linked, hardware_tried;
	GLuint gbuffer_program, gbuffer_framebuffer;
	GLint gbuffer_uniforms, gbuffer_depth;
	unsigned long world_generation;
	GLuint vertex_array;
	GLuint scene_texture, effect_texture;
	GLuint scene_framebuffer, effect_framebuffer, output_framebuffer, source_framebuffer;
	GLuint output_texture;
	int width, height;
	unsigned long frame;
} ray;

#ifdef HALO_GLES
#define SHADER_HEADER "#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;\n"
#else
#define SHADER_HEADER "#version 330 core\n"
#endif

static const char vertex_source[] =
	SHADER_HEADER
	"void main()\n"
	"{\n"
	"	vec2 corner = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
	"	gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);\n"
	"}\n";

/* the shared part: positions from depth. u[0] = (near, far, tan of half
the vertical field of view, aspect), u[1] = (viewport x, y, width, height),
u[2] = (radius, occlusion, reflection, bounce), u[3] = (frame, texture
width, texture height, 0) */
/* the rays are traced at half the resolution (every other pixel each
way), and the composite blends them back up across edges by depth */
#define TRACE_SCALE 2
#define TRACE_SCALE_TEXT "2"

#define COMMON_SOURCE \
	"#define TRACE_SCALE " TRACE_SCALE_TEXT "\n" \
	"uniform vec4 u[4];\n" \
	"uniform sampler2D depth_texture;\n" \
	"float linear_depth(float d)\n" \
	"{\n" \
	"	float n = u[0].x, f = u[0].y;\n" \
	"	return n * f / (f - d * (f - n));\n" \
	"}\n" \
	"float depth_at(ivec2 p)\n" \
	"{\n" \
	"	p = clamp(p, ivec2(u[1].xy), ivec2(u[1].xy + u[1].zw) - 1);\n" \
	"	return texelFetch(depth_texture, p, 0).r;\n" \
	"}\n" \
	"vec3 view_position(vec2 pixel, float z)\n" \
	"{\n" \
	"	vec2 ndc = (pixel - u[1].xy) / u[1].zw * 2.0 - 1.0;\n" \
	"	return vec3(ndc.x * u[0].z * u[0].w * z, ndc.y * u[0].z * z, z);\n" \
	"}\n" \
	"vec3 position_at(ivec2 p)\n" \
	"{\n" \
	"	return view_position(vec2(p) + 0.5, linear_depth(depth_at(p)));\n" \
	"}\n" \
	"vec2 project(vec3 v)\n" \
	"{\n" \
	"	vec2 ndc = vec2(v.x / (v.z * u[0].z * u[0].w), v.y / (v.z * u[0].z));\n" \
	"	return u[1].xy + (ndc * 0.5 + 0.5) * u[1].zw;\n" \
	"}\n"

/* the traced light, denoised on the rays' grid, after they are traced: an
a-trous wavelet filter (as SVGF's, Schied et al. 2017), three passes of 5x5
taps each twice as far apart as the last (1, 2, 4 of the rays' pixels),
which together reach 29 across - fewer as the pixel's samples grow (the
lights' texture's first: how many). Each tap weighs by the B3 spline, and less
the farther its depth and facing are from this pixel's (the gbuffer's), and
the farther its light's brightness is from this one's, measured against how
much the light varies here (3x3) - noise is smoothed, an edge in the light
(a shadow's) kept. u[0]: the grid; u[1]: the taps' step, whether the light
is the lights' texture (the first pass: gba, where the results' r is 2)
rather than the last pass's (rgb, a 1 where there is some) */
static const char denoise_source[] =
	SHADER_HEADER
	"uniform sampler2D lights_texture;\n"
	"uniform sampler2D results_texture;\n"
	"uniform sampler2D gbuffer_texture;\n"
	"uniform sampler2D counts_texture;\n"
	"uniform vec4 u[4];\n"
	"out vec4 result;\n"
	"ivec2 lo, hi;\n"
	"vec4 light_at(ivec2 k)\n"
	"{\n"
	"	if (u[1].y > 0.5)\n"
	"		return texelFetch(results_texture, k, 0).r < 1.5 ? vec4(0.0) : vec4(texelFetch(lights_texture, k, 0).gba, 1.0);\n"
	"	return texelFetch(lights_texture, k, 0);\n"
	"}\n"
	"float brightness(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }\n"
	"void main()\n"
	"{\n"
	"	ivec2 q0 = ivec2(gl_FragCoord.xy);\n"
	"	lo = ivec2(u[0].xy); hi = max(lo, ivec2(u[0].xy + u[0].zw) - 1);\n"
	"	vec4 g0 = texelFetch(gbuffer_texture, q0, 0);\n"
	"	vec4 l0 = light_at(q0);\n"
	"	if (g0.x <= 0.0 || l0.a <= 0.0) { result = vec4(0.0); return; }\n"
	/* (how much the light varies here: its brightness's, 3x3) */
	"	float m1 = 0.0, m2 = 0.0, n = 0.0;\n"
	"	for (int y = -1; y <= 1; y++)\n"
	"		for (int x = -1; x <= 1; x++)\n"
	"		{\n"
	"			vec4 l = light_at(clamp(q0 + ivec2(x, y), lo, hi));\n"
	"			if (l.a <= 0.0) continue;\n"
	"			float b = brightness(l.rgb);\n"
	"			m1 += b; m2 += b * b; n += 1.0;\n"
	"		}\n"
	"	m1 /= n; m2 /= n;\n"
	"	float spread = 4.0 * sqrt(max(m2 - m1 * m1, 0.0)) + 0.02 + 0.1 * m1;\n"
	"	float b0 = brightness(l0.rgb);\n"
	"	int step = int(u[1].x);\n"
	/* (a pixel of many samples wants little of it: past 8, the taps 2 apart
	   at most; past 24, 1) */
	"	float held = texelFetch(counts_texture, q0, 0).r;\n"
	"	if ((held > 24.0 && step > 1) || (held > 8.0 && step > 2)) { result = vec4(l0.rgb, 1.0); return; }\n"
	"	float h[5] = float[5](0.0625, 0.25, 0.375, 0.25, 0.0625);\n"
	"	vec3 sum = vec3(0.0);\n"
	"	float total = 0.0;\n"
	"	for (int y = -2; y <= 2; y++)\n"
	"		for (int x = -2; x <= 2; x++)\n"
	"		{\n"
	"			ivec2 k = clamp(q0 + ivec2(x, y) * step, lo, hi);\n"
	"			vec4 g = texelFetch(gbuffer_texture, k, 0);\n"
	"			vec4 l = light_at(k);\n"
	"			if (g.x <= 0.0 || l.a <= 0.0) continue;\n"
	"			float w = h[x + 2] * h[y + 2];\n"
	"			w *= exp(-abs(g.x - g0.x) / (g0.x * 0.012 * float(step) + 0.01));\n"
	"			w *= pow(max(dot(g.yzw, g0.yzw), 0.0), 32.0);\n"
	"			w *= exp(-abs(brightness(l.rgb) - b0) / spread);\n"
	"			sum += l.rgb * w;\n"
	"			total += w;\n"
	"		}\n"
	"	result = total > 0.0 ? vec4(sum / total, 1.0) : vec4(l0.rgb, 1.0);\n"
	"}\n";

/* the traced light into the light buffer, in place of the lightmaps', on
the level's pixels (not the objects', drawn before, whole: each pixel's
depth then is its depth now). The last frame's lighting pass traced and
denoised it (Metal's writes are not yet seen this early in the frame):
each pixel's point, from its depth and this camera, found in the last
frame's view (cameras: this one's position, forward, up, right, then the
last's; tan and aspect in their w), and the light there, between its four
rays' pixels (bilinear), each taken where its depth is the point's */
static const char inject_source[] =
	SHADER_HEADER
	COMMON_SOURCE
	"uniform sampler2D irradiance_texture;\n"
	"uniform sampler2D gbuffer_texture;\n"
	"uniform sampler2D objects_texture;\n"
	"uniform vec4 cameras[8];\n"
	"uniform vec4 previous_grid;\n"
	"uniform int split;\n"
	/* (the light where there is none at all: the level's average, lit) */
	"uniform vec4 fallback;\n"
	"layout(location = 0) out vec4 result;\n"
	/* (and what it puts there, for the composite) */
	"layout(location = 1) out vec4 applied;\n"
	"void main()\n"
	"{\n"
	"	ivec2 p = ivec2(gl_FragCoord.xy);\n"
	/* (split: the game's light on the left half, the traced on the right) */
	"	if (split != 0 && float(p.x) < u[1].x + u[1].z * 0.5) discard;\n"
	"	float d = depth_at(p);\n"
	"	if (d >= 0.99999) discard;\n"
	"	vec4 o = texelFetch(objects_texture, p, 0);\n"
	"	float object = dot(floor(o.rgb * 255.0 + 0.5), vec3(1.0, 256.0, 65536.0)) / 16777215.0;\n"
	"	if (abs(object - d) < 4.0 / 16777215.0) discard;\n"
	"	float z = linear_depth(d);\n"
	"	float t = cameras[0].w, aspect = cameras[1].w;\n"
	"	vec2 grid_origin = u[1].xy / float(TRACE_SCALE), grid_size = u[1].zw / float(TRACE_SCALE);\n"
	"	vec2 ndc = ((vec2(p) + 0.5) / float(TRACE_SCALE) - grid_origin) / grid_size * 2.0 - 1.0;\n"
	"	vec3 P = cameras[0].xyz + cameras[1].xyz * z + cameras[3].xyz * (ndc.x * t * aspect * z) - cameras[2].xyz * (ndc.y * t * z);\n"
	"	vec3 rel = P - cameras[4].xyz;\n"
	"	float pz = dot(rel, cameras[5].xyz);\n"
	/* (behind the last camera - turned right round: where it is on the
	   screen now, in the last frame's view) */
	"	vec2 pn = pz > u[0].x ? vec2(dot(rel, cameras[7].xyz) / (pz * cameras[4].w * cameras[5].w), -dot(rel, cameras[6].xyz) / (pz * cameras[4].w)) : ndc;\n"
	"	pz = max(pz, u[0].x);\n"
	"	vec2 q = previous_grid.xy + (pn * 0.5 + 0.5) * previous_grid.zw - 0.5;\n"
	"	ivec2 lo = ivec2(previous_grid.xy), hi = max(lo, ivec2(previous_grid.xy + previous_grid.zw) - 1);\n"
	"	ivec2 base = ivec2(floor(q));\n"
	"	vec2 f = q - floor(q);\n"
	"	vec3 sum = vec3(0.0);\n"
	"	float total = 0.0;\n"
	"	for (int y = 0; y <= 1; y++)\n"
	"		for (int x = 0; x <= 1; x++)\n"
	"		{\n"
	"			ivec2 k = clamp(base + ivec2(x, y), lo, hi);\n"
	"			float g = texelFetch(gbuffer_texture, k, 0).x;\n"
	"			vec4 light = texelFetch(irradiance_texture, k, 0);\n"
	"			if (g <= 0.0 || light.a < 0.5 || abs(g - pz) > pz * 0.05 + 0.05) continue;\n"
	"			float w = (x == 0 ? 1.0 - f.x : f.x) * (y == 0 ? 1.0 - f.y : f.y) + 1e-4;\n"
	"			sum += light.rgb * w;\n"
	"			total += w;\n"
	"		}\n"
	/* (split 2, HALO_RT_INJECT_DEBUG: green where there is traced light) */
	/* (none there - a place the last frame did not see, beside an edge: the
	   nearest like it, 5x5 about, less strictly by depth) */
	"	for (int y = -2; y <= 2 && total <= 0.0; y++)\n"
	"		for (int x = -2; x <= 2; x++)\n"
	"		{\n"
	"			ivec2 k = clamp(base + ivec2(x, y), lo, hi);\n"
	"			float g = texelFetch(gbuffer_texture, k, 0).x;\n"
	"			vec4 light = texelFetch(irradiance_texture, k, 0);\n"
	"			if (g <= 0.0 || light.a < 0.5 || abs(g - pz) > pz * 0.2 + 0.1) continue;\n"
	"			float w = 1.0 / (1.0 + float(x * x + y * y));\n"
	"			sum += light.rgb * w;\n"
	"			total += w;\n"
	"		}\n"
	/* (and none like it - come into view as the camera turned, off the last
	   frame's edge: the light nearest it there, whatever its depth, until
	   the rays reach it - with the lightmaps left out, black otherwise) */
	"	for (int y = -2; y <= 2 && total <= 0.0; y++)\n"
	"		for (int x = -2; x <= 2; x++)\n"
	"		{\n"
	"			ivec2 k = clamp(base + ivec2(x, y) * 2, lo, hi);\n"
	"			vec4 light = texelFetch(irradiance_texture, k, 0);\n"
	"			if (light.a < 0.5) continue;\n"
	"			sum += light.rgb;\n"
	"			total += 1.0;\n"
	"		}\n"
	"	if (split == 2) { result = vec4(0.0, total > 0.0 ? 1.0 : 0.0, 0.0, 1.0); return; }\n"
	"	if (total <= 0.0) { result = vec4(fallback.rgb, 1.0); applied = vec4(fallback.rgb + 1.0, 1.0); return; }\n"
	"	result = vec4(min(sum / total, vec3(1.0)), 1.0);\n"
	/* (plus 1: a pixel it took nothing for stays 0, the alpha being masked) */
	"	applied = vec4(result.rgb + 1.0, 1.0);\n"
	"}\n";

static const char trace_source[] =
	SHADER_HEADER
	COMMON_SOURCE
	"uniform sampler2D scene_texture;\n"
	"out vec4 result;\n"
	"float noise(vec2 p)\n"
	"{\n"
	"	return fract(52.9829189 * fract(dot(p + u[3].x * vec2(5.588238, 7.231), vec2(0.06711056, 0.00583715))));\n"
	"}\n"
	"void main()\n"
	"{\n"
	"	ivec2 p = ivec2(gl_FragCoord.xy) * TRACE_SCALE;\n"
	"	float d = depth_at(p);\n"
	"	if (d >= 0.99999) { result = vec4(0.0, 0.0, 0.0, 1.0); return; }\n"
	"	vec3 P = position_at(p);\n"
	/* the normal: the flatter of the two differences on each axis, so
	   edges do not bend it */
	"	vec3 l = position_at(p - ivec2(1, 0)), r = position_at(p + ivec2(1, 0));\n"
	"	vec3 b = position_at(p - ivec2(0, 1)), t = position_at(p + ivec2(0, 1));\n"
	/* the side nearer in depth (not across an edge), inside the viewport:
	   at its border, the side outside is the pixel itself */
	"	ivec2 low = ivec2(u[1].xy), high = ivec2(u[1].xy + u[1].zw) - 1;\n"
	"	bool right = p.x >= high.x ? false : p.x <= low.x ? true : abs(r.z - P.z) < abs(P.z - l.z);\n"
	"	bool top = p.y >= high.y ? false : p.y <= low.y ? true : abs(t.z - P.z) < abs(P.z - b.z);\n"
	"	vec3 dx = right ? r - P : P - l;\n"
	"	vec3 dy = top ? t - P : P - b;\n"
	"	vec3 N = normalize(cross(dy, dx));\n"
	"	if (dot(N, P) > 0.0) N = -N;\n"
	"	float jitter = noise(gl_FragCoord.xy);\n"
	/* occlusion and bounce: 8 directions around the normal, 4 steps each,
	   within the radius (screen-space size from the depth) */
	"	float radius = u[2].x;\n"
	"	float pixels = radius / (P.z * u[0].z) * u[1].w * 0.5;\n"
	"	pixels = clamp(pixels, 2.0, 96.0);\n"
	"	float occlusion = 0.0;\n"
	"	vec3 bounce = vec3(0.0);\n"
	"	for (int i = 0; i < 8; i++)\n"
	"	{\n"
	"		float angle = (float(i) + jitter) * 0.78539816;\n"
	"		vec2 direction = vec2(cos(angle), sin(angle));\n"
	"		float horizon = 0.0;\n"
	"		for (int j = 1; j <= 4; j++)\n"
	"		{\n"
	"			float step_length = pixels * (float(j) - 0.5 + 0.5 * jitter) / 4.0;\n"
	"			ivec2 q = p + ivec2(direction * step_length);\n"
	"			vec3 S = position_at(q);\n"
	"			vec3 v = S - P;\n"
	"			float distance_squared = dot(v, v);\n"
	"			float cosine = dot(N, v) * inversesqrt(distance_squared + 1e-6);\n"
	"			float falloff = clamp(1.0 - distance_squared / (radius * radius), 0.0, 1.0);\n"
	"			float h = max(cosine - 0.1, 0.0) * falloff;\n"
	"			if (h > horizon)\n"
	"			{\n"
	"				bounce += texelFetch(scene_texture, clamp(q, ivec2(u[1].xy), ivec2(u[1].xy + u[1].zw) - 1), 0).rgb"
	" * (h - horizon);\n"
	"				horizon = h;\n"
	"			}\n"
	"		}\n"
	"		occlusion += horizon;\n"
	"	}\n"
	"	occlusion = clamp(occlusion / 8.0 * 1.6, 0.0, 1.0);\n"
	"	bounce /= 8.0;\n"
	/* reflection: march the mirror ray in view space */
	"	vec3 V = normalize(P);\n"
	"	vec3 R = reflect(V, N);\n"
	"	float fresnel = pow(1.0 - clamp(dot(-V, N), 0.0, 1.0), 5.0);\n"
	"	vec3 reflection = vec3(0.0);\n"
	"	float reflected = 0.0;\n"
	"	if (u[2].z > 0.0 && R.z > -0.5)\n"
	"	{\n"
	"		float travel = 0.02 * P.z * (1.0 + jitter);\n"
	"		vec3 ray = P + N * 0.01 * P.z;\n"
	"		for (int k = 0; k < 24; k++)\n"
	"		{\n"
	"			ray += R * travel;\n"
	"			travel *= 1.25;\n"
	"			if (ray.z <= u[0].x) break;\n"
	"			vec2 screen = project(ray);\n"
	"			if (any(lessThan(screen, u[1].xy)) || any(greaterThanEqual(screen, u[1].xy + u[1].zw))) break;\n"
	"			float scene_z = linear_depth(depth_at(ivec2(screen)));\n"
	"			float behind = ray.z - scene_z;\n"
	"			if (behind > 0.0 && behind < travel * 2.0 + 0.05 * scene_z)\n"
	"			{\n"
	"				vec2 edge = min(screen - u[1].xy, u[1].xy + u[1].zw - screen) / (u[1].zw * 0.1);\n"
	"				float fade = clamp(min(edge.x, edge.y), 0.0, 1.0) * (1.0 - float(k) / 24.0);\n"
	"				reflection = texelFetch(scene_texture, ivec2(screen), 0).rgb;\n"
	"				reflected = fade;\n"
	"				break;\n"
	"			}\n"
	"		}\n"
	"	}\n"
	"	float weight = reflected * mix(0.04, 1.0, fresnel) * u[2].z;\n"
	"	vec3 light = reflection * weight + bounce * u[2].w;\n"
	"	result = vec4(light, 1.0 - occlusion * u[2].y);\n"
	"}\n";

/* each pixel's linear depth and view-space normal, for the world-space
rays: x, y, z of the normal with x right, y down, z into the screen */
static const char gbuffer_source[] =
	SHADER_HEADER
	COMMON_SOURCE
	"uniform sampler2D objects_texture;\n"
	"uniform int objects_known;\n"
	"out vec4 result;\n"
	"void main()\n"
	"{\n"
	"	ivec2 p = ivec2(gl_FragCoord.xy) * TRACE_SCALE;\n"
	"	float d = depth_at(p);\n"
	"	if (d >= 0.99999) { result = vec4(0.0); return; }\n"
	"	vec3 P = position_at(p);\n"
	"	vec3 l = position_at(p - ivec2(1, 0)), r = position_at(p + ivec2(1, 0));\n"
	"	vec3 b = position_at(p - ivec2(0, 1)), t = position_at(p + ivec2(0, 1));\n"
	/* the side nearer in depth (not across an edge), inside the viewport:
	   at its border, the side outside is the pixel itself */
	"	ivec2 low = ivec2(u[1].xy), high = ivec2(u[1].xy + u[1].zw) - 1;\n"
	"	bool right = p.x >= high.x ? false : p.x <= low.x ? true : abs(r.z - P.z) < abs(P.z - l.z);\n"
	"	bool top = p.y >= high.y ? false : p.y <= low.y ? true : abs(t.z - P.z) < abs(P.z - b.z);\n"
	"	vec3 dx = right ? r - P : P - l;\n"
	"	vec3 dy = top ? t - P : P - b;\n"
	"	vec3 N = normalize(cross(dy, dx));\n"
	"	if (dot(N, P) > 0.0) N = -N;\n"
	/* an object's pixel (its depth when the objects were drawn is its
	   depth now): the depth negative */
	"	if (objects_known != 0)\n"
	"	{\n"
	"		vec4 o = texelFetch(objects_texture, ivec2(gl_FragCoord.xy) * TRACE_SCALE, 0);\n"
	"		float object = dot(floor(o.rgb * 255.0 + 0.5), vec3(1.0, 256.0, 65536.0)) / 16777215.0;\n"
	"		if (abs(object - d) < 4.0 / 16777215.0) P.z = -P.z;\n"
	"	}\n"
	"	result = vec4(P.z, N);\n"
	"}\n";

/* the objects' depth, packed in 24 bits (at the window's resolution: the
light buffer tells each pixel's objects from the level's) */
static const char objects_source[] =
	SHADER_HEADER
	COMMON_SOURCE
	"out vec4 result;\n"
	"void main()\n"
	"{\n"
	"	float d = clamp(depth_at(ivec2(gl_FragCoord.xy)), 0.0, 1.0);\n"
	"	uint v = uint(d * 16777215.0 + 0.5);\n"
	"	result = vec4(float(v & 255u), float((v >> 8) & 255u), float(v >> 16), 255.0) / 255.0;\n"
	"}\n";

static const char composite_source[] =
	SHADER_HEADER
	COMMON_SOURCE
	"uniform sampler2D scene_texture;\n"
	"uniform sampler2D effect_texture;\n"
	"uniform sampler2D rt_texture;\n"
	"uniform int rt_enabled;\n"
	"uniform int debug_mode;\n"
	"uniform sampler2D baked_texture;\n"
	"uniform sampler2D lit_texture;\n"
	"uniform int light_split;\n"
	"uniform sampler2D lit_rt;\n"
	/* (this frame's traced light, denoised on the rays' grid; what the light
	buffer took; the objects' depth; the rays' depth; whether to) */
	"uniform sampler2D denoised_texture;\n"
	"uniform sampler2D applied_texture;\n"
	"uniform sampler2D objects_depth;\n"
	"uniform sampler2D gbuffer_now;\n"
	"uniform int correct;\n"
	"out vec4 result;\n"
	"void main()\n"
	"{\n"
	"	ivec2 p = ivec2(gl_FragCoord.xy);\n"
	"	vec4 scene = texelFetch(scene_texture, p, 0);\n"
	"	float d = depth_at(p);\n"
	"	if (debug_mode == 3) { float z = linear_depth(d); result = vec4(vec3(fract(z / 10.0)), 1.0); return; }\n"
	/* the ray view (Metal's rays from the camera), on all the screen or its
	   right half, a line between */
	"	if (debug_mode == 4 || (debug_mode == 5 && float(p.x) >= u[1].x + u[1].z * 0.5))\n"
	"	{\n"
	"		if (debug_mode == 5 && float(p.x) < u[1].x + u[1].z * 0.5 + 2.0) { result = vec4(1.0); return; }\n"
	"		result = rt_enabled != 0 ? vec4(texelFetch(rt_texture, p / TRACE_SCALE, 0).rgb, 1.0) :\n"
	"			vec4(0.4, 0.0, 0.4, 1.0);\n"
	"		return;\n"
	"	}\n"
	"	if (d >= 0.99999) { result = scene; return; }\n"
	/* the level's pixels, put right to this frame's traced light: the
	   light buffer took the last frame's (moved with the camera; where none
	   was, the game's), so each pixel is scaled by this frame's over what it
	   took - no lag, and no gaps where the view opens up */
	"	if (correct != 0)\n"
	"	{\n"
	"		vec4 o = texelFetch(objects_depth, p, 0);\n"
	"		float object = dot(floor(o.rgb * 255.0 + 0.5), vec3(1.0, 256.0, 65536.0)) / 16777215.0;\n"
	"		if (abs(object - d) >= 4.0 / 16777215.0)\n"
	"		{\n"
	"			float z = linear_depth(d);\n"
	"			vec2 q = (vec2(p) + 0.5) / float(TRACE_SCALE) - 0.5;\n"
	"			ivec2 base = ivec2(floor(q));\n"
	"			vec2 f = q - floor(q);\n"
	"			ivec2 lo = ivec2(u[1].xy) / TRACE_SCALE, hi = max(lo, (ivec2(u[1].xy + u[1].zw) + TRACE_SCALE - 1) / TRACE_SCALE - 1);\n"
	"			vec3 now = vec3(0.0);\n"
	"			float total = 0.0;\n"
	"			for (int y = 0; y <= 1; y++)\n"
	"				for (int x = 0; x <= 1; x++)\n"
	"				{\n"
	"					ivec2 k = clamp(base + ivec2(x, y), lo, hi);\n"
	"					float g = texelFetch(gbuffer_now, k, 0).x;\n"
	"					vec4 light = texelFetch(denoised_texture, k, 0);\n"
	"					if (g <= 0.0 || light.a < 0.5 || abs(g - z) > z * 0.05 + 0.05) continue;\n"
	"					float w = (x == 0 ? 1.0 - f.x : f.x) * (y == 0 ? 1.0 - f.y : f.y) + 1e-4;\n"
	"					now += light.rgb * w;\n"
	"					total += w;\n"
	"				}\n"
	"			if (total > 0.0)\n"
	"			{\n"
	"				vec4 took = texelFetch(applied_texture, p, 0);\n"
	/* (none taken: the game's light, as the light stages kept it) */
	"				vec3 was = took.r > 0.5 ? took.rgb - 1.0 : texelFetch(lit_texture, p / TRACE_SCALE, 0).rgb;\n"
	"				now = min(now / total, vec3(1.0));\n"
	"				scene.rgb *= clamp((now + 0.02) / (was + 0.02), vec3(0.25), vec3(4.0));\n"
	"			}\n"
	"		}\n"
	"	}\n"
	/* the occlusion blurred over 4x4 of the rays' pixels (8x8 of the
	   window's at half resolution: all 16 of the rays' sets of directions)
	   of similar depth */
	"	float z = linear_depth(d);\n"
	"	float total = 0.0, visibility = 0.0, dynamic_visibility = 0.0;\n"
	"	vec3 emitted = vec3(0.0);\n"
	"	vec3 light = vec3(0.0);\n"
	"	for (int y = -2; y < 2; y++)\n"
	"		for (int x = -2; x < 2; x++)\n"
	"		{\n"
	"			ivec2 q = clamp(p + ivec2(x, y) * TRACE_SCALE, ivec2(u[1].xy), ivec2(u[1].xy + u[1].zw) - 1);\n"
	/* (a pixel 10% nearer or farther counts a quarter; across an edge,
	   hardly) */
	"			float w = 1.0 / (1.0 + abs(linear_depth(texelFetch(depth_texture, q, 0).r) - z) / z * 30.0);\n"
	"			vec4 e = texelFetch(effect_texture, q / TRACE_SCALE, 0);\n"
	"			float v = e.a;\n"
	/* the world's occlusion (Metal's rays) with the screen's (which also
	   finds the objects) */
	"			if (rt_enabled != 0)\n"
	/* (2: a level pixel the traced light was put in the light buffer of,
	   whose occlusion that light has) */
	"			{\n"
	"				float traced = texelFetch(rt_texture, q / TRACE_SCALE, 0).r;\n"
	"				v = traced > 1.5 ? 1.0 : v * mix(1.0, traced, u[2].y);\n"
	"			}\n"
	"			visibility += v * w;\n"
	"			vec4 lit_here = rt_enabled != 0 ? texelFetch(lit_rt, q / TRACE_SCALE, 0) : vec4(1.0, 0.0, 0.0, 0.0);\n"
	/* (a pixel whose light is in the light buffer: its lights' texture holds
	   that light, not the lights') */
	"			if (rt_enabled != 0 && texelFetch(rt_texture, q / TRACE_SCALE, 0).r > 1.5) lit_here = vec4(1.0, 0.0, 0.0, 0.0);\n"
	"			dynamic_visibility += lit_here.r * w;\n"
	"			emitted += lit_here.gba * w;\n"
	"			light += e.rgb * w;\n"
	"			total += w;\n"
	"		}\n"
	"	visibility /= total;\n"
	"	dynamic_visibility /= total;\n"
	"	emitted /= total;\n"
	"	light /= total;\n"
	/* the lightmaps' share of the light: the occlusion and the sun's
	   shadows darken it, not the flashlight's or the other dynamic lights'
	   (objects, drawn before the lightmaps, have it all) */
	"	if (light_split != 0)\n"
	"	{\n"
	"		const vec3 luma = vec3(0.3, 0.59, 0.11);\n"
	"		float baked = dot(texelFetch(baked_texture, p / TRACE_SCALE, 0).rgb, luma);\n"
	"		float lit = dot(texelFetch(lit_texture, p / TRACE_SCALE, 0).rgb, luma);\n"
	/* (the dynamic lights' share: its traced shadows, the flashlight's
	   and the others') */
	"		float share = lit > 0.004 ? clamp(baked / lit, 0.0, 1.0) : 1.0;\n"
	"		visibility = share * visibility + (1.0 - share) * dynamic_visibility;\n"
	"	}\n"
	/* a traced reflection, where the camera sees what it hit, before the
	   screen's */
	"	if (rt_enabled != 0)\n"
	"	{\n"
	"		vec4 hit = texelFetch(rt_texture, p / TRACE_SCALE, 0);\n"
	/* (a: how much, with the Fresnel term; the screen's rays traced no
	   reflection: u[2].z was 0 for them) */
	"		if (hit.a > 0.0)\n"
	"			light += texelFetch(scene_texture, ivec2(hit.gb * u[3].yz), 0).rgb * hit.a * u[3].w;\n"
	"	}\n"
	"	if (debug_mode == 2) { result = vec4(vec3(visibility), 1.0); return; }\n"
	/* the emitters' light: tinting the lit surface, and a little of its own
	   on the dark */
	"	result = vec4(scene.rgb * visibility + light * (1.0 - scene.rgb * 0.5) + emitted * (scene.rgb * 1.5 + 0.12), scene.a);\n"
	"}\n";

static GLuint compile(GLenum type, const char *source, const char *what)
{
	GLuint shader = glCreateShader(type);
	GLint status = 0;

	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);
	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if (!status)
	{
		char log[4096];

		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		platform_log("ray tracing: cannot compile the %s shader:\n%s", what, log);
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

static GLuint link_with(const char *vertex_text, const char *fragment, const char *what)
{
	GLuint vertex = compile(GL_VERTEX_SHADER, vertex_text, "vertex");
	GLuint pixel = compile(GL_FRAGMENT_SHADER, fragment, what);
	GLuint program;
	GLint status = 0;

	if (!vertex || !pixel)
		return 0;
	program = glCreateProgram();
	glAttachShader(program, vertex);
	glAttachShader(program, pixel);
	glLinkProgram(program);
	glDeleteShader(vertex);
	glDeleteShader(pixel);
	glGetProgramiv(program, GL_LINK_STATUS, &status);
	if (!status)
	{
		char log[4096];

		glGetProgramInfoLog(program, sizeof(log), NULL, log);
		platform_log("ray tracing: cannot link the %s program:\n%s", what, log);
		return 0;
	}
	return program;
}

static GLuint link(const char *fragment, const char *what)
{
	return link_with(vertex_source, fragment, what);
}

/* ---------- the ray probe

The rays the lighting sends from the surface at the crosshair (Metal's: the
kernel writes them, host_rt_probe reads them back), drawn as lines in the
world: the occlusion rays white, the ray to the sun yellow, the reflection
cyan, the rays to the dynamic lights (the flashlight's) orange, each red
where it hit; the normal green. Frozen, they stay where they
were while the camera moves round them. Where the scene is nearer than a
line, the line is faint. */

#define PROBE_SEGMENTS 15

static const char probe_vertex_source[] =
	SHADER_HEADER
	"uniform vec4 seg_a[15];\n" /* from, kind */
	"uniform vec4 seg_b[15];\n" /* to, whether it hit */
	"uniform vec4 eye;\n"
	"uniform vec4 ahead;\n"
	"uniform vec4 above;\n"
	"uniform vec4 across;\n"
	"uniform vec4 lens;\n" /* near, far, tan of half the vertical field of view, aspect */
	"uniform vec4 pixels;\n" /* the viewport's width and height */
	"out vec4 line_color;\n"
	"out float view_z;\n"
	"vec3 to_view(vec3 w) { vec3 v = w - eye.xyz; return vec3(dot(v, across.xyz), dot(v, above.xyz), dot(v, ahead.xyz)); }\n"
	"vec4 to_clip(vec3 v)\n"
	"{\n"
	"	float z = v.z;\n"
	"	float depth = lens.y / (lens.y - lens.x) * (1.0 - lens.x / z);\n"
	/* (rows from the top, as the game's targets hold them) */
	"	return vec4(v.x / (lens.z * lens.w), -v.y / lens.z, (depth * 2.0 - 1.0) * z, z);\n"
	"}\n"
	"void main()\n"
	"{\n"
	"	int s = gl_VertexID / 6, corner = gl_VertexID - s * 6;\n"
	"	vec3 a = to_view(seg_a[s].xyz), b = to_view(seg_b[s].xyz);\n"
	"	float kind = seg_a[s].w, hit = seg_b[s].w;\n"
	"	float nearest = lens.x * 2.0;\n"
	"	if (a.z < nearest && b.z < nearest) { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); line_color = vec4(0.0); view_z = 0.0; return; }\n"
	"	if (a.z < nearest) a = mix(a, b, (nearest - a.z) / (b.z - a.z));\n"
	"	if (b.z < nearest) b = mix(b, a, (nearest - b.z) / (a.z - b.z));\n"
	"	vec4 ca = to_clip(a), cb = to_clip(b);\n"
	"	vec2 direction = (cb.xy / cb.w - ca.xy / ca.w) * pixels.xy;\n"
	"	direction = length(direction) > 1e-4 ? normalize(direction) : vec2(1.0, 0.0);\n"
	"	float thickness = kind > 2.5 ? 2.0 : 3.0;\n"
	"	vec2 side = vec2(-direction.y, direction.x) * thickness * 2.0 / pixels.xy;\n"
	/* two triangles: a-, b-, b+ and a-, b+, a+ */
	"	bool at_b = corner == 1 || corner == 2 || corner == 4;\n"
	"	float sign_side = (corner == 0 || corner == 1 || corner == 3) ? -1.0 : 1.0;\n"
	"	vec4 c = at_b ? cb : ca;\n"
	"	c.xy += side * sign_side * c.w;\n"
	"	gl_Position = c;\n"
	"	view_z = at_b ? b.z : a.z;\n"
	"	vec3 color = kind < 0.5 ? (hit > 0.5 ? vec3(1.0, 0.25, 0.2) : vec3(1.0)) :\n"
	"		kind < 1.5 ? (hit > 0.5 ? vec3(1.0, 0.15, 0.15) : vec3(1.0, 0.9, 0.2)) :\n"
	"		kind < 2.5 ? (hit > 0.5 ? vec3(0.2, 1.0, 1.0) : vec3(0.5, 0.75, 0.8)) :\n"
	"		kind < 3.5 ? vec3(0.3, 1.0, 0.3) : kind < 4.5 ? (hit > 0.5 ? vec3(1.0, 0.15, 0.15) : vec3(1.0, 0.55, 0.1)) :\n"
	"		(hit > 0.5 ? vec3(1.0, 0.15, 0.15) : vec3(1.0, 0.35, 0.9));\n"
	"	line_color = vec4(color, 1.0);\n"
	"}\n";

static const char probe_pixel_source[] =
	SHADER_HEADER
	COMMON_SOURCE
	"in vec4 line_color;\n"
	"in float view_z;\n"
	"out vec4 result;\n"
	"void main()\n"
	"{\n"
	"	float scene = linear_depth(texelFetch(depth_texture, ivec2(gl_FragCoord.xy), 0).r);\n"
	/* behind the scene: faint */
	"	float alpha = view_z > scene * 1.02 + 0.02 ? 0.3 : 1.0;\n"
	"	result = vec4(line_color.rgb, alpha);\n"
	"}\n";

static struct
{
	int mode;	/* 0 off, 1 live, 2 frozen */
	GLuint program;
	GLint seg_a, seg_b, eye, ahead, above, across, lens, pixels, u, depth;
	float segments[PROBE_SEGMENTS * 8];
	int count;
} probe;

static void initialize(void);

/* F4: the objects' shapes in the rays next - the drawn models, the
collision models; returns their name */
const char *halo_ray_tracing_shapes_next(void)
{
	static const char *const names[] = { "the drawn models", "the collision models" };

	if (!ray.initialized)
		initialize();
	ray.shapes = (ray.shapes + 1) % 2;
	return names[ray.shapes];
}

/* F5: the ray probe off, live (the crosshair's rays), frozen (where they
were); returns what it is now */
const char *halo_ray_tracing_probe_next(void)
{
	probe.mode = (probe.mode + 1) % 3;
	if (probe.mode == 1)
		probe.count = 0;
	return probe.mode == 0 ? "off" : probe.mode == 1 ? "the crosshair's rays" : "frozen (walk round them)";
}

static void probe_draw(GLuint color, GLuint depth, const int *viewport, const float *uniforms, const float *position,
	const float *forward, const float *up)
{
	const GLenum draw_buffer = GL_COLOR_ATTACHMENT0;
	float a[PROBE_SEGMENTS * 4], b[PROBE_SEGMENTS * 4], vector[4], right[3], length;
	int index;

	if (!probe.mode || probe.count <= 0 || !position || !forward || !up)
		return;
	if (!probe.program)
	{
		probe.program = link_with(probe_vertex_source, probe_pixel_source, "ray probe");
		if (!probe.program)
		{
			probe.mode = 0;
			return;
		}
		probe.seg_a = glGetUniformLocation(probe.program, "seg_a");
		probe.seg_b = glGetUniformLocation(probe.program, "seg_b");
		probe.eye = glGetUniformLocation(probe.program, "eye");
		probe.ahead = glGetUniformLocation(probe.program, "ahead");
		probe.above = glGetUniformLocation(probe.program, "above");
		probe.across = glGetUniformLocation(probe.program, "across");
		probe.lens = glGetUniformLocation(probe.program, "lens");
		probe.pixels = glGetUniformLocation(probe.program, "pixels");
		probe.u = glGetUniformLocation(probe.program, "u");
		probe.depth = glGetUniformLocation(probe.program, "depth_texture");
	}
	for (index = 0; index < PROBE_SEGMENTS; index++)
	{
		const float *segment = &probe.segments[index * 8];
		int k;

		for (k = 0; k < 4; k++)
		{
			a[index * 4 + k] = index < probe.count ? segment[k] : 0.0f;
			b[index * 4 + k] = index < probe.count ? segment[4 + k] : 0.0f;
		}
	}
	right[0] = forward[1] * up[2] - forward[2] * up[1];
	right[1] = forward[2] * up[0] - forward[0] * up[2];
	right[2] = forward[0] * up[1] - forward[1] * up[0];
	length = sqrtf(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
	if (length <= 0.0f)
		return;
	glBindFramebuffer(GL_FRAMEBUFFER, ray.output_framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
	glDrawBuffers(1, &draw_buffer);
	glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
	glScissor(viewport[0], viewport[1], viewport[2], viewport[3]);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glUseProgram(probe.program);
	glUniform4fv(probe.seg_a, PROBE_SEGMENTS, a);
	glUniform4fv(probe.seg_b, PROBE_SEGMENTS, b);
	vector[3] = 0.0f;
	memcpy(vector, position, 3 * sizeof(float));
	glUniform4fv(probe.eye, 1, vector);
	memcpy(vector, forward, 3 * sizeof(float));
	glUniform4fv(probe.ahead, 1, vector);
	memcpy(vector, up, 3 * sizeof(float));
	glUniform4fv(probe.above, 1, vector);
	vector[0] = right[0] / length;
	vector[1] = right[1] / length;
	vector[2] = right[2] / length;
	glUniform4fv(probe.across, 1, vector);
	glUniform4fv(probe.lens, 1, uniforms);
	vector[0] = (float)viewport[2];
	vector[1] = (float)viewport[3];
	vector[2] = vector[3] = 0.0f;
	glUniform4fv(probe.pixels, 1, vector);
	glUniform4fv(probe.u, 4, uniforms);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, depth);
	glUniform1i(probe.depth, 1);
	glDrawArrays(GL_TRIANGLES, 0, probe.count * 6);
	glDisable(GL_BLEND);
}

static int mode_from_setting(const char *text)
{
	if (!strcmp(text, "off") || !strcmp(text, "false"))
		return _ray_tracing_off;
	if (!strcmp(text, "occlusion"))
		return _ray_tracing_debug_occlusion;
	if (!strcmp(text, "depth"))
		return _ray_tracing_debug_depth;
	if (!strcmp(text, "rays"))
		return _ray_tracing_debug_rays;
	if (!strcmp(text, "split"))
		return _ray_tracing_debug_split;
	if (!strcmp(text, "screen"))
		return _ray_tracing_on;
	return _ray_tracing_on;
}

/* Metal's rays' programs (macOS), once: ray.hardware_linked if they are
there */
static void hardware_link(void)
{
	if (ray.hardware_tried)
		return;
	ray.hardware_tried = 1;
#ifdef HALO_MACOS
	if (host_rt_available())
	{
		ray.gbuffer_program = link(gbuffer_source, "ray tracing depth and normals");
		if (ray.gbuffer_program)
		{
			ray.gbuffer_uniforms = glGetUniformLocation(ray.gbuffer_program, "u");
			ray.gbuffer_depth = glGetUniformLocation(ray.gbuffer_program, "depth_texture");
			ray.gbuffer_objects = glGetUniformLocation(ray.gbuffer_program, "objects_texture");
			ray.gbuffer_objects_known = glGetUniformLocation(ray.gbuffer_program, "objects_known");
			ray.denoise_program = link_with(vertex_source, denoise_source, "ray tracing light buffer denoise");
			if (ray.denoise_program)
			{
				ray.denoise_uniforms = glGetUniformLocation(ray.denoise_program, "u");
				ray.denoise_lights = glGetUniformLocation(ray.denoise_program, "lights_texture");
				ray.denoise_results = glGetUniformLocation(ray.denoise_program, "results_texture");
				ray.denoise_gbuffer = glGetUniformLocation(ray.denoise_program, "gbuffer_texture");
				ray.denoise_counts = glGetUniformLocation(ray.denoise_program, "counts_texture");
			}
			ray.inject_program = link(inject_source, "ray tracing light buffer");
			if (ray.inject_program)
			{
				ray.inject_uniforms = glGetUniformLocation(ray.inject_program, "u");
				ray.inject_depth = glGetUniformLocation(ray.inject_program, "depth_texture");
				ray.inject_irradiance = glGetUniformLocation(ray.inject_program, "irradiance_texture");
				ray.inject_gbuffer = glGetUniformLocation(ray.inject_program, "gbuffer_texture");
				ray.inject_split = glGetUniformLocation(ray.inject_program, "split");
				ray.inject_fallback = glGetUniformLocation(ray.inject_program, "fallback");
				ray.inject_results = glGetUniformLocation(ray.inject_program, "results_texture");
				ray.inject_objects = glGetUniformLocation(ray.inject_program, "objects_texture");
				ray.inject_cameras = glGetUniformLocation(ray.inject_program, "cameras");
				ray.inject_grid = glGetUniformLocation(ray.inject_program, "previous_grid");
			}
			ray.objects_program = link(objects_source, "ray tracing objects' depth");
			if (ray.objects_program)
			{
				ray.objects_uniforms = glGetUniformLocation(ray.objects_program, "u");
				ray.objects_depth = glGetUniformLocation(ray.objects_program, "depth_texture");
			}
			glGenFramebuffers(1, &ray.gbuffer_framebuffer);
			ray.hardware = 1;
			ray.hardware_linked = 1;
		}
	}
#endif
}

static void initialize(void)
{
	ray.initialized = 1;
	ray.mode = mode_from_setting(config_string("display.ray_tracing"));
	ray.enabled = ray.mode != _ray_tracing_off;
	ray.occlusion_strength = (float)config_real("display.ray_tracing_occlusion");
	ray.reflection_strength = (float)config_real("display.ray_tracing_reflections");
	ray.bounce_strength = (float)config_real("display.ray_tracing_bounce");
	ray.shadow_strength = (float)config_real("display.ray_tracing_shadows");
	ray.objects = config_boolean("display.ray_tracing_objects");
	{
		const char *shapes = config_string("display.ray_tracing_shapes");

		ray.shapes = !strcmp(shapes, "collision") ? 1 : 0;
	}
	ray.traced_lights = strcmp(config_string("display.ray_tracing_lights"), "game") != 0;
	ray.drawn_level = strcmp(config_string("display.ray_tracing_level"), "collision") != 0;
	{
		const char *gi = config_string("display.ray_tracing_gi");

		ray.gi = !strcmp(gi, "traced") ? 1 : !strcmp(gi, "black") ? 2 : !strcmp(gi, "path") ? 3 : 0;
	}
	ray.gi_sun = (float)config_real("display.ray_tracing_gi_sun");
	ray.gi_bounce = (float)config_real("display.ray_tracing_gi_bounce");
	ray.gi_glow = (float)config_real("display.ray_tracing_gi_glow");
	ray.gi_bounces = (float)config_real("display.ray_tracing_bounces");
	ray.gi_samples = (float)config_real("display.ray_tracing_samples");
	ray.gi_lights = (float)config_real("display.ray_tracing_gi_lights");
	ray.gi_split = config_boolean("display.ray_tracing_gi_split");
	/* no drawing (debug.null_renderer: headless tests, bots) has no GL */
	if (config_boolean("debug.null_renderer") || !glCreateShader)
	{
		ray.failed = 1;
		ray.enabled = 0;
		return;
	}
	/* world units (a world unit is about 3 m) */
	ray.radius = 0.35f;
	ray.trace_program = link(trace_source, "ray tracing");
	ray.composite_program = link(composite_source, "ray tracing composite");
	if (!ray.trace_program || !ray.composite_program)
	{
		ray.failed = 1;
		platform_log("ray tracing: unavailable");
		return;
	}
	ray.trace_uniforms = glGetUniformLocation(ray.trace_program, "u");
	ray.trace_scene = glGetUniformLocation(ray.trace_program, "scene_texture");
	ray.trace_depth = glGetUniformLocation(ray.trace_program, "depth_texture");
	ray.composite_uniforms = glGetUniformLocation(ray.composite_program, "u");
	ray.composite_scene = glGetUniformLocation(ray.composite_program, "scene_texture");
	ray.composite_depth = glGetUniformLocation(ray.composite_program, "depth_texture");
	ray.composite_effect = glGetUniformLocation(ray.composite_program, "effect_texture");
	ray.composite_debug = glGetUniformLocation(ray.composite_program, "debug_mode");
	ray.composite_rt = glGetUniformLocation(ray.composite_program, "rt_texture");
	ray.composite_rt_enabled = glGetUniformLocation(ray.composite_program, "rt_enabled");
	ray.composite_baked = glGetUniformLocation(ray.composite_program, "baked_texture");
	ray.composite_lit = glGetUniformLocation(ray.composite_program, "lit_texture");
	ray.composite_light_split = glGetUniformLocation(ray.composite_program, "light_split");
	ray.composite_lit_rt = glGetUniformLocation(ray.composite_program, "lit_rt");
	ray.composite_denoised = glGetUniformLocation(ray.composite_program, "denoised_texture");
	ray.composite_applied = glGetUniformLocation(ray.composite_program, "applied_texture");
	ray.composite_objects = glGetUniformLocation(ray.composite_program, "objects_depth");
	ray.composite_gbuffer = glGetUniformLocation(ray.composite_program, "gbuffer_now");
	ray.composite_correct = glGetUniformLocation(ray.composite_program, "correct");
	/* "screen" keeps to the screen's rays (Metal's are made ready when they
	are asked for: halo_ray_tracing_set) */
	if (strcmp(config_string("display.ray_tracing"), "screen"))
		hardware_link();
	glGenVertexArrays(1, &ray.vertex_array);
	glGenFramebuffers(1, &ray.scene_framebuffer);
	glGenFramebuffers(1, &ray.effect_framebuffer);
	glGenFramebuffers(1, &ray.output_framebuffer);
	glGenFramebuffers(1, &ray.source_framebuffer);
	glGenFramebuffers(1, &ray.light_framebuffer);
	platform_log("ray tracing: %s, %s (F9 switches it; occlusion %.2f, reflections %.2f, bounce %.2f)",
		ray.enabled ? "on" : "off", ray.hardware ? "full (rays through the whole level, on Metal)" : "lite (screen-space rays only)",
		ray.occlusion_strength, ray.reflection_strength, ray.bounce_strength);
}

/* half floats, drawn to (the traced light, denoised) */
static GLuint make_float_texture(int width, int height)
{
	GLuint texture;

	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_HALF_FLOAT, NULL);
	return texture;
}

static GLuint make_texture(int width, int height)
{
	GLuint texture;

	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	return texture;
}

/* the scene copy and the effect texture, at the targets' size */
static void size_textures(int width, int height)
{
	if (ray.width == width && ray.height == height)
		return;
	if (ray.scene_texture)
	{
		glDeleteTextures(1, &ray.scene_texture);
		glDeleteTextures(1, &ray.effect_texture);
		glDeleteTextures(1, &ray.baked_texture);
		glDeleteTextures(1, &ray.lit_texture);
		glDeleteTextures(1, &ray.objects_texture);
		glDeleteTextures(1, &ray.denoised_texture);
		glDeleteTextures(1, &ray.denoising_texture);
		glDeleteTextures(1, &ray.applied_texture);
	}
	ray.scene_texture = make_texture(width, height);
	ray.effect_texture = make_texture((width + TRACE_SCALE - 1) / TRACE_SCALE, (height + TRACE_SCALE - 1) / TRACE_SCALE);
	ray.baked_texture = make_texture((width + TRACE_SCALE - 1) / TRACE_SCALE, (height + TRACE_SCALE - 1) / TRACE_SCALE);
	ray.lit_texture = make_texture((width + TRACE_SCALE - 1) / TRACE_SCALE, (height + TRACE_SCALE - 1) / TRACE_SCALE);
	ray.objects_texture = make_texture(width, height);
	ray.denoised_texture = make_float_texture((width + TRACE_SCALE - 1) / TRACE_SCALE, (height + TRACE_SCALE - 1) / TRACE_SCALE);
	ray.denoising_texture = make_float_texture((width + TRACE_SCALE - 1) / TRACE_SCALE, (height + TRACE_SCALE - 1) / TRACE_SCALE);
	ray.applied_texture = make_float_texture(width, height);
	ray.light_stages = 0;
	ray.width = width;
	ray.height = height;
	glBindFramebuffer(GL_FRAMEBUFFER, ray.scene_framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ray.scene_texture, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, ray.effect_framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ray.effect_texture, 0);
}

const char *halo_ray_tracing_toggle(void)
{
	if (!ray.initialized)
		initialize();
	ray.enabled = !ray.enabled;
	if (ray.enabled && ray.mode == _ray_tracing_off)
		ray.mode = _ray_tracing_on;
	platform_log("ray tracing: %s", ray.enabled ? "on" : "off");
	if (!ray.enabled)
		return "off";
	if (ray.failed)
		return "unavailable";
	return ray.hardware ? "on (full: rays through the whole level)" : "on (lite: screen-space rays only)";
}

/* F6: what it shows next - the lighting, the ray view, the lighting and the
ray view side by side, the occlusion; returns its name */
const char *halo_ray_tracing_next_view(void)
{
	static const int order[] = { _ray_tracing_on, _ray_tracing_debug_rays, _ray_tracing_debug_split,
		_ray_tracing_debug_occlusion };
	static const char *const names[] = { "lighting", "ray view (what the rays hit)",
		"split (lighting | ray view)", "occlusion" };
	int index, next = 0;

	if (!ray.initialized)
		initialize();
	for (index = 0; index < 4; index++)
	{
		if (ray.enabled && ray.mode == order[index])
			next = (index + 1) % 4;
	}
	ray.mode = order[next];
	ray.enabled = 1;
	if (!ray.hardware && (ray.mode == _ray_tracing_debug_rays || ray.mode == _ray_tracing_debug_split))
		return "ray view needs full ray tracing (lite is on)";
	return names[next];
}

/* what it shows: 1 the lighting, 2 the occlusion, 3 the depth, 4 the ray
view, 5 split (tests) */
void halo_ray_tracing_debug_mode(int mode)
{
	if (!ray.initialized)
		initialize();
	ray.mode = mode;
	ray.enabled = mode != _ray_tracing_off;
}

/* the views in the settings overlay's order (settings_overlay.c), as F6's */
static const int settings_views[] = { _ray_tracing_on, _ray_tracing_debug_rays, _ray_tracing_debug_split,
	_ray_tracing_debug_occlusion, _ray_tracing_debug_depth };

/* the settings as they are now (the overlay's rows) */
void halo_ray_tracing_get(struct halo_ray_tracing_settings *settings)
{
	int index;

	if (!ray.initialized)
		initialize();
	memset(settings, 0, sizeof(*settings));
	settings->tracing = !ray.enabled ? 0 : ray.hardware ? 1 : 2;
	for (index = 0; index < (int)(sizeof(settings_views) / sizeof(settings_views[0])); index++)
	{
		if (ray.mode == settings_views[index])
			settings->view = index;
	}
	settings->gi = ray.gi;
	settings->traced_lights = ray.traced_lights;
	settings->shapes = ray.shapes;
	settings->objects = ray.objects;
	settings->gi_split = ray.gi_split;
	settings->occlusion = ray.occlusion_strength;
	settings->reflections = ray.reflection_strength;
	settings->bounce = ray.bounce_strength;
	settings->shadows = ray.shadow_strength;
	settings->gi_sun = ray.gi_sun;
	settings->gi_bounce = ray.gi_bounce;
	settings->gi_glow = ray.gi_glow;
	settings->gi_bounces = ray.gi_bounces;
	settings->gi_samples = ray.gi_samples;
	settings->gi_lights = ray.gi_lights;
	settings->hardware_available = ray.hardware_linked || !ray.hardware_tried;
	settings->failed = ray.failed;
}

/* takes them up at once. What the traced light has gathered over the last
frames is dropped when what it traces changes (the traced light's mode, the
lights, the objects' shapes, Metal's rays on or off): the next frame starts
it again, as the first frame of a level does */
void halo_ray_tracing_set(const struct halo_ray_tracing_settings *settings)
{
	int hardware, restart, was_hardware;

	if (!ray.initialized)
		initialize();
	/* (making Metal's programs sets ray.hardware) */
	was_hardware = ray.hardware;
	if (settings->tracing == 1)
		hardware_link();
	/* (off keeps which rays F9 turns back on: Metal's, or the screen's) */
	hardware = settings->tracing == 1 ? ray.hardware_linked : settings->tracing == 2 ? 0 : ray.hardware;
	restart = hardware != was_hardware || settings->gi != ray.gi || settings->traced_lights != ray.traced_lights ||
		settings->shapes != ray.shapes || settings->objects != ray.objects || (settings->tracing != 0) != ray.enabled;
	ray.enabled = settings->tracing != 0;
	ray.hardware = hardware;
	if (settings->view >= 0 && settings->view < (int)(sizeof(settings_views) / sizeof(settings_views[0])))
		ray.mode = settings_views[settings->view];
	if (ray.mode == _ray_tracing_off)
		ray.mode = _ray_tracing_on;
	ray.gi = settings->gi;
	ray.traced_lights = settings->traced_lights;
	ray.shapes = settings->shapes;
	ray.objects = settings->objects;
	ray.gi_split = settings->gi_split;
	ray.occlusion_strength = settings->occlusion;
	ray.reflection_strength = settings->reflections;
	ray.bounce_strength = settings->bounce;
	ray.shadow_strength = settings->shadows;
	ray.gi_sun = settings->gi_sun;
	ray.gi_bounce = settings->gi_bounce;
	ray.gi_glow = settings->gi_glow;
	ray.gi_bounces = settings->gi_bounces;
	ray.gi_samples = settings->gi_samples;
	ray.gi_lights = settings->gi_lights;
	if (restart)
	{
		/* (no last camera: Metal's kernel takes none of its history; no last
		frame's rays for the light buffer, which takes the game's light for
		a frame) */
		ray.previous_camera[12] = 0.0f;
		ray.gi_previous = 0;
		ray.gi_traced = 0;
		ray.light_stages = 0;
		platform_log("ray tracing: %s, traced light %d, lights %s, shapes %s, objects %s (the traced light starts again)",
			!ray.enabled ? "off" : ray.hardware ? "on" : "the screen's rays", ray.gi,
			ray.traced_lights ? "traced" : "game", ray.shapes ? "collision" : "model", ray.objects ? "on" : "off");
	}
}

#ifdef HALO_MACOS
/* the level's rays with Metal: the depth and normals into the shared
texture, the rays, and the results' texture; 0 if not */
/* the light probes: the points the objects' lighting asked about this
frame (for the rays), and the last probes done (10 floats each: the point,
the light, how much from one way, that way) */
static struct
{
	float requests[64 * 3];
	int request_count;
	float results[64 * 10];
	int result_count;
} probes;

static GLuint world_rays(const float *uniforms, const float *position, const float *forward, const float *up,
	int width, int height, GLuint depth)
{
	const GLenum draw_buffer = GL_COLOR_ATTACHMENT0;
	const float *vertices;
	const unsigned long *indices;
	long vertex_count, triangle_count;
	unsigned long generation;
	GLuint input, output;
	float camera[96], right[3], length;

	generation = halo_ray_tracing_world(&vertices, &vertex_count, &indices, &triangle_count);
	if (!generation)
		return 0;
	if (generation != ray.world_generation)
	{
		ray.world_generation = generation;
		host_rt_set_world(generation, vertices, (int)vertex_count, (const unsigned int *)indices, (int)triangle_count);
	}
	/* the drawn level, its materials as their colours are read, and its
	lightmap pages as the texture cache loads them (two a frame) */
	if (ray.drawn_level)
	{
		const float *level_vertices, *texcoords, *base_texcoords, *materials;
		const unsigned long *level_indices, *triangle_materials;
		long level_vertex_count, level_triangle_count, material_count, page, page_width, page_height, step, cutout_start;
		const unsigned char *pixels;
		unsigned long level_generation = halo_ray_tracing_level(&level_vertices, &texcoords, &base_texcoords,
			&level_vertex_count, &level_indices, &triangle_materials, &level_triangle_count, &cutout_start);

		if (level_generation && level_generation != ray.level_generation)
		{
			ray.level_generation = level_generation;
			ray.lightmap_sum[0] = ray.lightmap_sum[1] = ray.lightmap_sum[2] = ray.lightmap_samples = 0.0;
			host_rt_set_level(level_generation, level_vertices, texcoords, base_texcoords, (int)level_vertex_count,
				(const unsigned int *)level_indices, (const unsigned int *)triangle_materials, (int)level_triangle_count,
				(int)cutout_start);
		}
		if (level_generation)
		{
			if (halo_ray_tracing_level_materials(&materials, &material_count))
				host_rt_set_level_materials(materials, (int)material_count);
			/* (the cutouts' masks, four a frame) */
			{
				long mask, mask_width, mask_height;
				const unsigned char *alpha;
				unsigned long mask_generation;

				for (step = 0; step < 4 && halo_ray_tracing_mask(&mask, &alpha, &mask_width, &mask_height,
					&mask_generation); step++)
				{
					host_rt_set_mask((unsigned int)mask_generation, (int)mask, (int)mask_width, (int)mask_height, alpha);
				}
			}
			for (step = 0; step < 2 && halo_ray_tracing_level_page(&page, &pixels, &page_width, &page_height); step++)
			{
				static int pages;

				host_rt_set_level_page((int)page, (int)page_width, (int)page_height, pixels);
				pages++;
				{
					long texel;

					for (texel = 0; texel < page_width * page_height; texel += 7)
					{
						ray.lightmap_sum[0] += pixels[texel * 4 + 0] / 255.0;
						ray.lightmap_sum[1] += pixels[texel * 4 + 1] / 255.0;
						ray.lightmap_sum[2] += pixels[texel * 4 + 2] / 255.0;
						ray.lightmap_samples += 1.0;
					}
				}
				if (pages <= 3 || (pages & 15) == 0)
					platform_log("ray tracing: lightmap page %ld (%ldx%ld), %d so far", page, page_width, page_height, pages);
			}
		}
	}
	width = (width + TRACE_SCALE - 1) / TRACE_SCALE;
	height = (height + TRACE_SCALE - 1) / TRACE_SCALE;
	input = host_rt_texture(0, width, height);
	output = host_rt_texture(1, width, height);
	ray.lights_texture = host_rt_texture(2, width, height);
	/* (the traced light: in the lights' texture, on the level's pixels) */
	ray.irradiance_texture = ray.gi ? ray.lights_texture : 0;
	/* (the host's texture creation binds on the active unit) */
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, depth);
	if (!input || !output || !ray.lights_texture)
		return 0;
	ray.gbuffer_texture = input;
	glBindFramebuffer(GL_FRAMEBUFFER, ray.gbuffer_framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, input, 0);
	glDrawBuffers(1, &draw_buffer);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
	{
		platform_log("ray tracing: the shared depth texture cannot be drawn to; world-space rays off");
		ray.hardware = 0;
		ray.hardware_linked = 0;
		return 0;
	}
	glUseProgram(ray.gbuffer_program);
	glUniform4fv(ray.gbuffer_uniforms, 4, uniforms);
	glUniform1i(ray.gbuffer_depth, 1);
	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, ray.objects_texture);
	glBindSampler(2, 0);
	glUniform1i(ray.gbuffer_objects, 2);
	glUniform1i(ray.gbuffer_objects_known, (ray.light_stages & 4) != 0);
	glDrawArrays(GL_TRIANGLES, 0, 3);

	/* the camera: its right is forward x up (the game's world is
	right-handed, z up) */
	right[0] = forward[1] * up[2] - forward[2] * up[1];
	right[1] = forward[2] * up[0] - forward[0] * up[2];
	right[2] = forward[0] * up[1] - forward[1] * up[0];
	length = sqrtf(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
	if (length <= 0.0f)
		return 0;
	memcpy(camera, position, 3 * sizeof(float));
	memcpy(camera + 3, forward, 3 * sizeof(float));
	memcpy(camera + 6, up, 3 * sizeof(float));
	camera[9] = right[0] / length;
	camera[10] = right[1] / length;
	camera[11] = right[2] / length;
	camera[12] = uniforms[0];
	camera[13] = uniforms[1];
	camera[14] = uniforms[2];
	camera[15] = uniforms[3];
	/* the viewport on the half-resolution grid */
	camera[16] = uniforms[4] / TRACE_SCALE;
	camera[17] = uniforms[5] / TRACE_SCALE;
	camera[18] = uniforms[6] / TRACE_SCALE;
	camera[19] = uniforms[7] / TRACE_SCALE;
	camera[20] = (float)(ray.frame & 1023);
	/* world units: about 3 m each */
	camera[21] = 0.6f;
	camera[22] = 40.0f;
	/* the objects' pixels, marked in the depth and normals */
	camera[23] = (ray.light_stages & 4) ? 1.0f : 0.0f;
	/* the ray view, the ray probe */
	camera[32] = ray.mode == _ray_tracing_debug_rays ? 1.0f : ray.mode == _ray_tracing_debug_split ? 2.0f : 0.0f;
	camera[33] = probe.mode == 1 ? 1.0f : 0.0f;
	camera[34] = ray.traced_lights ? 1.0f : 0.0f;
	camera[35] = 0.0f;
	/* the traced light (host_metal_rt.m): the sun's and the sky's colours
	and powers, the mode, the parts' strengths, the frame, the drawn level,
	the last frame's camera */
	memset(camera + 36, 0, 48 * sizeof(float));
	/* the lightmaps' average light, for surfaces whose page is missing
	(80-82; a grey until there are pages) */
	camera[80] = camera[81] = camera[82] = 0.25f;
	if (ray.lightmap_samples > 0.0)
	{
		camera[80] = (float)(ray.lightmap_sum[0] / ray.lightmap_samples);
		camera[81] = (float)(ray.lightmap_sum[1] / ray.lightmap_samples);
		camera[82] = (float)(ray.lightmap_sum[2] / ray.lightmap_samples);
	}
	memcpy(ray.lightmap_average, camera + 80, sizeof(ray.lightmap_average));
	{
		static int frames;
		long done, total;

		halo_ray_tracing_level_pages(&done, &total);
		if (ray.drawn_level && ray.gi && done < total && (frames++ % 1800) == 900)
			platform_log("ray tracing: lightmap pages %ld of %ld (the rest light as their average)", done, total);
	}
	{
		float sky[25];

		if (halo_ray_tracing_sky(sky))
		{
			static int logged;

			memcpy(camera + 36, sky + 3, 6 * sizeof(float));
			/* the sky's wide lights: 64-79 */
			memcpy(camera + 64, sky + 9, 16 * sizeof(float));
			if (!logged)
			{
				logged = 1;
				platform_log("ray tracing: the sky's sun %.2f %.2f %.2f, ambient %.2f %.2f %.2f",
					sky[3], sky[4], sky[5], sky[6], sky[7], sky[8]);
			}
		}
	}
	camera[42] = (float)ray.gi;
	camera[43] = ray.gi_bounce;
	camera[44] = (float)(ray.frame & 65535);
	camera[45] = ray.gi_sun;
	camera[46] = ray.drawn_level ? 1.0f : 0.0f;
	camera[47] = ray.gi_glow;
	/* (the path tracer's bounces at most, and the traced light's rays a pixel
	each frame it takes new ones) */
	camera[92] = (float)ray.gi_bounces;
	camera[93] = (float)ray.gi_samples;
	memcpy(camera + 48, ray.previous_camera, 13 * sizeof(float));
	camera[61] = ray.gi_lights;
	/* (how much of each new sample the accumulated light takes, at least:
	the average of the last 33 or so) */
	camera[62] = getenv("HALO_RT_GI_BLEND") ? (float)atof(getenv("HALO_RT_GI_BLEND")) : 0.03f;
	/* (the traced light's new samples every this many frames a pixel: 0,
	the host's governor chooses) */
	camera[63] = getenv("HALO_RT_GI_PERIOD") ? (float)atof(getenv("HALO_RT_GI_PERIOD")) : 0.0f;
	/* the sun, for shadows on the objects */
	camera[27] = halo_ray_tracing_sun(camera + 24) ? ray.shadow_strength : 0.0f;
	/* the objects, as shapes for the rays */
	{
		/* (their triangles, at most the host's 65536) */
		static float triangles[65536 * 9];
		static unsigned char groups[65536];
		/* (and their cutouts: 8 floats each) */
		static float cutouts[65536 * 8];
		long count = ray.objects ?
			halo_ray_tracing_objects(triangles, groups, cutouts, getenv("HALO_RT_OBJECT_TRIANGLES") ? atol(getenv("HALO_RT_OBJECT_TRIANGLES")) : 65536, position, camera + 28, ray.shapes) : 0;

		if (!ray.objects)
			camera[31] = 0.0f;

		/* (the drawn models are traced from both sides: their winding is not
		kept to their outsides as the collision models' is) */
		host_rt_set_objects(triangles, groups, cutouts, (int)count, ray.shapes == 0);
	}
	/* the lights: traced, all of them, or the game's dynamic ones, for their
	shadows */
	{
		static float lights[64 * 12];
		long light_count = halo_ray_tracing_lights(lights, 64, ray.traced_lights), i, j;

		/* the 16 nearest (to their reach's edge: a wide light far off can
		still reach you), first */
		for (i = 0; i < light_count && i < 16; i++)
		{
			long nearest = i;
			float best = 1e30f;

			for (j = i; j < light_count; j++)
			{
				const float *l = lights + j * 12;
				float dx = l[0] - position[0], dy = l[1] - position[1], dz = l[2] - position[2];
				float edge = sqrtf(dx * dx + dy * dy + dz * dz) - l[3];

				if (edge < best)
				{
					best = edge;
					nearest = j;
				}
			}
			if (nearest != i)
			{
				float swap[12];

				memcpy(swap, lights + i * 12, sizeof(swap));
				memcpy(lights + i * 12, lights + nearest * 12, sizeof(swap));
				memcpy(lights + nearest * 12, swap, sizeof(swap));
			}
		}
		if (light_count > 16)
			light_count = 16;

		host_rt_set_lights(lights, (int)light_count);
	}
	/* the emitters (glowing projectiles), for their light */
	{
		static float emitters[16 * 8];
		long emitter_count = ray.objects ? halo_ray_tracing_emitters(emitters, 16, position) : 0;

		host_rt_set_emitters(emitters, (int)emitter_count);
	}
	/* the probes asked for this frame, traced with the rays */
	host_rt_set_probes(probes.requests, probes.request_count);
	probes.request_count = 0;
	if (!host_rt_trace(camera, width, height))
		return 0;
	probes.result_count = host_rt_probe_results(probes.results, 64);
	/* (this camera, for the next frame's) */
	memcpy(ray.previous_camera, camera, 12 * sizeof(float));
	ray.previous_camera[12] = 1.0f;
	/* (the probe's rays: the last frame's, which the GPU has written) */
	if (probe.mode == 1)
		probe.count = host_rt_probe(probe.segments, PROBE_SEGMENTS);
	return output;
}
#endif

void halo_ray_traced_light_stage(int stage)
{
	GLuint color, depth;
	int width, height, viewport[4];

	if (!ray.initialized)
		initialize();
	if (!ray.enabled || ray.failed || stage < 0 || stage > 2)
		return;
	if (!xgpu_current_targets(&color, &depth, &width, &height, viewport) || !color || viewport[2] < 16 ||
		viewport[3] < 16)
	{
		return;
	}
	size_textures(width, height);
	if (stage == 2)
	{
		/* the objects' depth, on the rays' grid (Metal's rays: their pixels) */
		const GLenum draw_buffer = GL_COLOR_ATTACHMENT0;
		float uniforms[16] = { 0 };

		if (!ray.hardware || !ray.objects_program || !depth)
			return;
		uniforms[4] = (float)viewport[0];
		uniforms[5] = (float)viewport[1];
		uniforms[6] = (float)viewport[2];
		uniforms[7] = (float)viewport[3];
		glDisable(GL_DEPTH_TEST);
		glDisable(GL_STENCIL_TEST);
		glDisable(GL_BLEND);
		glDisable(GL_CULL_FACE);
		glDisable(GL_SCISSOR_TEST);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glBindFramebuffer(GL_FRAMEBUFFER, ray.light_framebuffer);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ray.objects_texture, 0);
		glDrawBuffers(1, &draw_buffer);
		glViewport(0, 0, width, height);
		glBindVertexArray(ray.vertex_array);
		glUseProgram(ray.objects_program);
		glUniform4fv(ray.objects_uniforms, 4, uniforms);
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, depth);
		glBindSampler(1, 0);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glUniform1i(ray.objects_depth, 1);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		glActiveTexture(GL_TEXTURE0);
		ray.light_stages |= 1 << 2;
		xgpu_gl_bind_device_vertex_array();
		xgpu_gl_state_invalidate();
		return;
	}
	/* the window's light, at half resolution */
	glDisable(GL_SCISSOR_TEST);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, ray.source_framebuffer);
	glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, ray.light_framebuffer);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
		stage == 0 ? ray.baked_texture : ray.lit_texture, 0);
	glBlitFramebuffer(viewport[0], viewport[1], viewport[0] + viewport[2], viewport[1] + viewport[3],
		viewport[0] / TRACE_SCALE, viewport[1] / TRACE_SCALE, (viewport[0] + viewport[2]) / TRACE_SCALE,
		(viewport[1] + viewport[3]) / TRACE_SCALE, GL_COLOR_BUFFER_BIT, GL_LINEAR);
	ray.light_stages |= 1 << stage;
	xgpu_gl_state_invalidate();
}

void halo_ray_traced_object_lighting(const float *position, float *color, float *normal, float *accuracy)
{
#ifdef HALO_MACOS
	int index, best = -1;
	float best_distance = 0.75f * 0.75f;

	if (!ray.enabled || ray.failed || !ray.gi || !ray.hardware || !position)
		return;
	for (index = 0; index < probes.request_count; index++)
	{
		const float *q = probes.requests + index * 3;
		float dx = q[0] - position[0], dy = q[1] - position[1], dz = q[2] - position[2];

		if (dx * dx + dy * dy + dz * dz < 0.25f * 0.25f)
			break;
	}
	if (index == probes.request_count && probes.request_count < 64)
	{
		memcpy(probes.requests + probes.request_count * 3, position, 3 * sizeof(float));
		probes.request_count++;
	}
	for (index = 0; index < probes.result_count; index++)
	{
		const float *r = probes.results + index * 10;
		float dx = r[0] - position[0], dy = r[1] - position[1], dz = r[2] - position[2];
		float distance = dx * dx + dy * dy + dz * dz;

		if (distance < best_distance)
		{
			best_distance = distance;
			best = index;
		}
	}
	if (best >= 0)
	{
		const float *r = probes.results + best * 10;

		color[0] = r[3] < 1.0f ? r[3] : 1.0f;
		color[1] = r[4] < 1.0f ? r[4] : 1.0f;
		color[2] = r[5] < 1.0f ? r[5] : 1.0f;
		*accuracy = r[6];
		normal[0] = r[7];
		normal[1] = r[8];
		normal[2] = r[9];
	}
#else
	(void)position;
	(void)color;
	(void)normal;
	(void)accuracy;
#endif
}

/* the lighting pass's uniforms (the composite's and the rays') */
static void lighting_uniforms(float *uniforms, float z_near, float z_far, float vertical_field_of_view,
	const int *viewport, int width, int height)
{
	uniforms[0] = z_near;
	uniforms[1] = z_far;
	uniforms[2] = tanf(vertical_field_of_view * 0.5f);
	uniforms[3] = (float)viewport[2] / (float)viewport[3];
	uniforms[4] = (float)viewport[0];
	uniforms[5] = (float)viewport[1];
	uniforms[6] = (float)viewport[2];
	uniforms[7] = (float)viewport[3];
	uniforms[8] = ray.radius;
	uniforms[9] = ray.occlusion_strength;
	uniforms[10] = ray.reflection_strength;
	uniforms[11] = ray.bounce_strength;
	uniforms[12] = (float)(ray.frame & 63);
	uniforms[13] = (float)width;
	uniforms[14] = (float)height;
	/* the reflections' strength for the composite; the screen's rays
	trace none when Metal's do */
	uniforms[15] = ray.reflection_strength;
	if (ray.hardware)
		uniforms[10] = 0.0f;
}

/* the traced light, denoised on the rays' grid (for the light buffer): the
rays' textures, the grid they were traced on, the camera's lens; FALSE if
there is none */
static int denoise_traced_light(GLuint world_results, const float *uniforms, int width, int height)
{
#ifdef HALO_MACOS
	ray.gi_previous = world_results && ray.gi && ray.lights_texture && ray.gbuffer_texture && ray.denoise_program &&
		ray.denoised_texture && ray.denoising_texture;
	if (ray.gi_previous)
	{
		/* the traced light, denoised on the rays' grid, for the next frame */
		float grid[16] = { 0 };
		const GLenum denoise_buffer = GL_COLOR_ATTACHMENT0;

		grid[0] = uniforms[4] / TRACE_SCALE;
		grid[1] = uniforms[5] / TRACE_SCALE;
		grid[2] = uniforms[6] / TRACE_SCALE;
		grid[3] = uniforms[7] / TRACE_SCALE;
		int pass;

		glBindFramebuffer(GL_FRAMEBUFFER, ray.light_framebuffer);
		glDrawBuffers(1, &denoise_buffer);
		glViewport(0, 0, (width + TRACE_SCALE - 1) / TRACE_SCALE, (height + TRACE_SCALE - 1) / TRACE_SCALE);
		glDisable(GL_SCISSOR_TEST);
		glUseProgram(ray.denoise_program);
		glActiveTexture(GL_TEXTURE2);
		glBindTexture(GL_TEXTURE_2D, world_results);
		glBindSampler(2, 0);
		glUniform1i(ray.denoise_results, 2);
		glActiveTexture(GL_TEXTURE3);
		glBindTexture(GL_TEXTURE_2D, ray.gbuffer_texture);
		glBindSampler(3, 0);
		glUniform1i(ray.denoise_gbuffer, 3);
		glActiveTexture(GL_TEXTURE4);
		glBindTexture(GL_TEXTURE_2D, ray.lights_texture);
		glBindSampler(4, 0);
		glUniform1i(ray.denoise_counts, 4);
		/* (the three passes: the lights' texture into the denoised, it into
		the one in between, and back) */
		for (pass = 0; pass < 3; pass++)
		{
			GLuint from = pass == 0 ? ray.lights_texture : (pass & 1) ? ray.denoised_texture : ray.denoising_texture;
			GLuint to = (pass & 1) ? ray.denoising_texture : ray.denoised_texture;

			grid[4] = (float)(1 << pass);
			grid[5] = pass == 0 ? 1.0f : 0.0f;
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, to, 0);
			glUniform4fv(ray.denoise_uniforms, 4, grid);
			glActiveTexture(GL_TEXTURE1);
			glBindTexture(GL_TEXTURE_2D, from);
			glBindSampler(1, 0);
			glUniform1i(ray.denoise_lights, 1);
			glDrawArrays(GL_TRIANGLES, 0, 3);
		}
		glEnable(GL_SCISSOR_TEST);
		ray.gi_results_texture = world_results;
		ray.gi_lights_texture = ray.denoised_texture;
		ray.gi_gbuffer_texture = ray.gbuffer_texture;
		ray.gi_grid[0] = uniforms[4] / TRACE_SCALE;
		ray.gi_grid[1] = uniforms[5] / TRACE_SCALE;
		ray.gi_grid[2] = uniforms[6] / TRACE_SCALE;
		ray.gi_grid[3] = uniforms[7] / TRACE_SCALE;
		ray.gi_tan = uniforms[2];
		ray.gi_aspect = uniforms[3];
	}
	glActiveTexture(GL_TEXTURE0);
	return ray.gi_previous;
#else
	(void)world_results;
	(void)uniforms;
	(void)width;
	(void)height;
	return 0;
#endif
}

/* after the game's lightmaps and dynamic lights, before the textures
multiply them in (render.c): with display.ray_tracing_gi, the rays traced
now, and their light put in the light buffer in place of the game's on the
level's pixels, for the textures to multiply as the lightmaps' */
int halo_ray_traced_lightmaps_hidden(void)
{
#ifdef HALO_MACOS
	/* (HALO_RT_KEEP_LIGHTMAPS: the traced light in their place, as before;
	and the split and its debugging keep them, for the left half) */
	return ray.initialized && ray.enabled && !ray.failed && ray.gi && ray.hardware && ray.inject_program &&
		ray.drawn_level && ray.gi_previous && (ray.light_stages & 4) && !ray.gi_split &&
		!getenv("HALO_RT_INJECT_DEBUG") && !getenv("HALO_RT_KEEP_LIGHTMAPS");
#else
	return 0;
#endif
}

void halo_ray_traced_light_buffer(float z_near, float z_far, float vertical_field_of_view, const float *position,
	const float *forward, const float *up)
{
#ifdef HALO_MACOS
	int adding = halo_ray_traced_lightmaps_hidden();

	GLuint color, depth;
	int width, height, viewport[4];
	float uniforms[16], cameras[32], right[3], length;
	const GLenum draw_buffer = GL_COLOR_ATTACHMENT0;

	if (!ray.initialized)
		initialize();
	ray.gi_traced = 0;
	if (!ray.enabled || ray.failed || !ray.gi || !ray.hardware || !ray.inject_program || !ray.drawn_level ||
		!(ray.light_stages & 4) || !(z_near > 0.0f) || !(z_far > z_near) ||
		!(vertical_field_of_view > 0.0f) || !position || !forward || !up)
	{
		return;
	}
	if (!xgpu_current_targets(&color, &depth, &width, &height, viewport) || !color || !depth ||
		viewport[2] < 16 || viewport[3] < 16)
	{
		return;
	}
	right[0] = forward[1] * up[2] - forward[2] * up[1];
	right[1] = forward[2] * up[0] - forward[0] * up[2];
	right[2] = forward[0] * up[1] - forward[1] * up[0];
	length = sqrtf(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
	if (length <= 0.0f)
		return;
	size_textures(width, height);
	lighting_uniforms(uniforms, z_near, z_far, vertical_field_of_view, viewport, width, height);
	/* (HALO_RT_GI_SAME_FRAME: the rays traced now, for this frame's light
	buffer - but GL does not see Metal's writes this early in the frame, so
	the last frame's are taken, and the composite puts this frame's right) */
	if (getenv("HALO_RT_GI_SAME_FRAME"))
	{
		GLuint results;

		glDisable(GL_DEPTH_TEST);
		glDisable(GL_STENCIL_TEST);
		glDisable(GL_BLEND);
		glDisable(GL_CULL_FACE);
		glEnable(GL_SCISSOR_TEST);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glViewport(0, 0, (width + TRACE_SCALE - 1) / TRACE_SCALE, (height + TRACE_SCALE - 1) / TRACE_SCALE);
		glScissor(viewport[0] / TRACE_SCALE, viewport[1] / TRACE_SCALE, (viewport[2] + TRACE_SCALE - 1) / TRACE_SCALE,
			(viewport[3] + TRACE_SCALE - 1) / TRACE_SCALE);
		glBindVertexArray(ray.vertex_array);
		results = world_rays(uniforms, position, forward, up, width, height, depth);
		if (results && denoise_traced_light(results, uniforms, width, height))
		{
			ray.gi_traced = 1;
			ray.gi_traced_results = results;
		}
	}
	if (!ray.gi_previous)
	{
		xgpu_gl_bind_device_vertex_array();
		xgpu_gl_state_invalidate();
		return;
	}
	/* this camera and the last frame's (ray.previous_camera: position,
	forward, up, right) */
	memset(cameras, 0, sizeof(cameras));
	memcpy(cameras + 0, position, 3 * sizeof(float));
	cameras[3] = uniforms[2];
	memcpy(cameras + 4, forward, 3 * sizeof(float));
	cameras[7] = uniforms[3];
	memcpy(cameras + 8, up, 3 * sizeof(float));
	cameras[12] = right[0] / length;
	cameras[13] = right[1] / length;
	cameras[14] = right[2] / length;
	memcpy(cameras + 16, ray.previous_camera + 0, 3 * sizeof(float));
	cameras[19] = ray.gi_tan;
	memcpy(cameras + 20, ray.previous_camera + 3, 3 * sizeof(float));
	cameras[23] = ray.gi_aspect;
	memcpy(cameras + 24, ray.previous_camera + 6, 3 * sizeof(float));
	memcpy(cameras + 28, ray.previous_camera + 9, 3 * sizeof(float));

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_CULL_FACE);
	glEnable(GL_SCISSOR_TEST);
	glViewport(0, 0, width, height);
	glScissor(viewport[0], viewport[1], viewport[2], viewport[3]);
	glBindVertexArray(ray.vertex_array);
	/* the light buffer's colour (not its alpha, the game's), and what goes
	in it, for the composite (cleared: none) */
	{
		const GLenum both[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };

		glBindFramebuffer(GL_FRAMEBUFFER, ray.light_framebuffer);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ray.applied_texture, 0);
		glDrawBuffers(1, &draw_buffer);
		glDisable(GL_SCISSOR_TEST);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glEnable(GL_SCISSOR_TEST);
		glBindFramebuffer(GL_FRAMEBUFFER, ray.output_framebuffer);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, ray.applied_texture, 0);
		glDrawBuffers(2, both);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
	}
	glUseProgram(ray.inject_program);
	glUniform4fv(ray.inject_uniforms, 4, uniforms);
	glUniform4fv(ray.inject_cameras, 8, cameras);
	glUniform4fv(ray.inject_grid, 1, ray.gi_grid);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, depth);
	glBindSampler(1, 0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glUniform1i(ray.inject_depth, 1);
	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, ray.gi_lights_texture);
	glBindSampler(2, 0);
	glUniform1i(ray.inject_irradiance, 2);
	glActiveTexture(GL_TEXTURE3);
	glBindTexture(GL_TEXTURE_2D, ray.gi_gbuffer_texture);
	glBindSampler(3, 0);
	glUniform1i(ray.inject_gbuffer, 3);
	glActiveTexture(GL_TEXTURE5);
	glBindTexture(GL_TEXTURE_2D, ray.objects_texture);
	glBindSampler(5, 0);
	glUniform1i(ray.inject_objects, 5);
	glUniform1i(ray.inject_split, getenv("HALO_RT_INJECT_DEBUG") ? 2 : ray.gi_split);
	{
		float fallback[4] = { ray.lightmap_average[0], ray.lightmap_average[1], ray.lightmap_average[2], 0.0f };

		glUniform4fv(ray.inject_fallback, 1, fallback);
	}
	/* (added to the self-illumination and the light decals, the lightmaps
	left out; or in the lightmaps' light's place) */
	if (adding)
	{
		glEnable(GL_BLEND);
		glBlendEquation(GL_FUNC_ADD);
		glBlendFunc(GL_ONE, GL_ONE);
	}
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glDisable(GL_BLEND);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	/* (the second target off again: the output framebuffer is the
	composite's too) */
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, 0, 0);
	ray.gi_applied = 1;
	glActiveTexture(GL_TEXTURE0);
	xgpu_gl_bind_device_vertex_array();
	xgpu_gl_state_invalidate();
#else
	(void)z_near;
	(void)z_far;
	(void)vertical_field_of_view;
	(void)position;
	(void)forward;
	(void)up;
#endif
}

void halo_ray_traced_lighting(float z_near, float z_far, float vertical_field_of_view, const float *position,
	const float *forward, const float *up)
{
	GLuint color, depth, world_results = 0;
	int width, height, viewport[4];
	float uniforms[16];
	const GLenum draw_buffer = GL_COLOR_ATTACHMENT0;

	if (!ray.initialized)
		initialize();
	if (!ray.enabled || ray.failed || !(z_near > 0.0f) || !(z_far > z_near) || !(vertical_field_of_view > 0.0f))
		return;
	if (!xgpu_current_targets(&color, &depth, &width, &height, viewport) || !color || !depth ||
		viewport[2] < 16 || viewport[3] < 16)
	{
		return;
	}
	ray.frame++;
	size_textures(width, height);

	/* the window's colour, copied: it is read and written */
	glBindFramebuffer(GL_READ_FRAMEBUFFER, ray.source_framebuffer);
	glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, ray.scene_framebuffer);
	glBlitFramebuffer(viewport[0], viewport[1], viewport[0] + viewport[2], viewport[1] + viewport[3],
		viewport[0], viewport[1], viewport[0] + viewport[2], viewport[1] + viewport[3],
		GL_COLOR_BUFFER_BIT, GL_NEAREST);

	lighting_uniforms(uniforms, z_near, z_far, vertical_field_of_view, viewport, width, height);

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_CULL_FACE);
	glEnable(GL_SCISSOR_TEST);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	/* the rays on the half-resolution grid */
	glViewport(0, 0, (width + TRACE_SCALE - 1) / TRACE_SCALE, (height + TRACE_SCALE - 1) / TRACE_SCALE);
	glScissor(viewport[0] / TRACE_SCALE, viewport[1] / TRACE_SCALE, (viewport[2] + TRACE_SCALE - 1) / TRACE_SCALE,
		(viewport[3] + TRACE_SCALE - 1) / TRACE_SCALE);
	glBindVertexArray(ray.vertex_array);

	/* the rays */
	glBindFramebuffer(GL_FRAMEBUFFER, ray.effect_framebuffer);
	glDrawBuffers(1, &draw_buffer);
	glUseProgram(ray.trace_program);
	glUniform4fv(ray.trace_uniforms, 4, uniforms);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, ray.scene_texture);
	glBindSampler(0, 0);
	glUniform1i(ray.trace_scene, 0);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, depth);
	glBindSampler(1, 0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glUniform1i(ray.trace_depth, 1);
	glDrawArrays(GL_TRIANGLES, 0, 3);

#ifdef HALO_MACOS
	/* (traced already this frame, for the light buffer) */
	if (ray.gi_traced)
		world_results = ray.gi_traced_results;
	else if (ray.hardware && position && forward && up)
		world_results = world_rays(uniforms, position, forward, up, width, height, depth);
	/* (for the next frame's light buffer: these rays, the grid they were
	traced on, the camera's lens) */
	if (!ray.gi_traced)
		denoise_traced_light(world_results, uniforms, width, height);
	ray.gi_traced = 0;
#else
	(void)position;
	(void)forward;
	(void)up;
#endif

	/* the composite, into the window's colour at its full resolution (its
	depth not attached, being read) */
	glViewport(0, 0, width, height);
	glScissor(viewport[0], viewport[1], viewport[2], viewport[3]);
	glBindFramebuffer(GL_FRAMEBUFFER, ray.output_framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
	glDrawBuffers(1, &draw_buffer);
	glUseProgram(ray.composite_program);
	glUniform4fv(ray.composite_uniforms, 4, uniforms);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, ray.scene_texture);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, depth);
	glUniform1i(ray.composite_scene, 0);
	glUniform1i(ray.composite_depth, 1);
	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, ray.effect_texture);
	glBindSampler(2, 0);
	glUniform1i(ray.composite_effect, 2);
	glUniform1i(ray.composite_debug, ray.mode);
	glActiveTexture(GL_TEXTURE3);
	glBindTexture(GL_TEXTURE_2D, world_results);
	glBindSampler(3, 0);
	glUniform1i(ray.composite_rt, 3);
	glUniform1i(ray.composite_rt_enabled, world_results != 0);
	glActiveTexture(GL_TEXTURE4);
	glBindTexture(GL_TEXTURE_2D, ray.baked_texture);
	glBindSampler(4, 0);
	glUniform1i(ray.composite_baked, 4);
	glActiveTexture(GL_TEXTURE5);
	glBindTexture(GL_TEXTURE_2D, ray.lit_texture);
	glBindSampler(5, 0);
	glUniform1i(ray.composite_lit, 5);
	glUniform1i(ray.composite_light_split, (ray.light_stages & 3) == 3);
	glActiveTexture(GL_TEXTURE6);
	glBindTexture(GL_TEXTURE_2D, world_results ? ray.lights_texture : 0);
	glBindSampler(6, 0);
	glUniform1i(ray.composite_lit_rt, 6);
	/* (the correction to this frame's traced light: when it was denoised this
	frame, the light buffer took some, and the objects' depth is known) */
	{
		int correct = ray.gi && world_results && ray.gi_previous && ray.gi_applied && (ray.light_stages & 7) == 7 &&
			ray.gbuffer_texture;

		glActiveTexture(GL_TEXTURE7);
		glBindTexture(GL_TEXTURE_2D, correct ? ray.denoised_texture : 0);
		glBindSampler(7, 0);
		glUniform1i(ray.composite_denoised, 7);
		glActiveTexture(GL_TEXTURE8);
		glBindTexture(GL_TEXTURE_2D, correct ? ray.applied_texture : 0);
		glBindSampler(8, 0);
		glUniform1i(ray.composite_applied, 8);
		glActiveTexture(GL_TEXTURE9);
		glBindTexture(GL_TEXTURE_2D, correct ? ray.objects_texture : 0);
		glBindSampler(9, 0);
		glUniform1i(ray.composite_objects, 9);
		glActiveTexture(GL_TEXTURE10);
		glBindTexture(GL_TEXTURE_2D, correct ? ray.gbuffer_texture : 0);
		glBindSampler(10, 0);
		glUniform1i(ray.composite_gbuffer, 10);
		glUniform1i(ray.composite_correct, correct && !getenv("HALO_RT_GI_NO_CORRECT"));
		ray.gi_applied = 0;
	}
	glDrawArrays(GL_TRIANGLES, 0, 3);
	ray.light_stages = 0;
	probe_draw(color, depth, viewport, uniforms, position, forward, up);

	/* the renderer's state is its own again */
	glActiveTexture(GL_TEXTURE0);
	xgpu_gl_bind_device_vertex_array();
	xgpu_gl_state_invalidate();
}
