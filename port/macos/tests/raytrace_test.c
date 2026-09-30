/*
RAYTRACE_TEST.C

Draws a scene as the game draws its frame, into textures like the game's
targets, and runs port/linux/src/raytrace_gl.c on it through ANGLE, as the
macOS port does:

1. the light: the level's baked light (the lightmaps: a sky's ambient and
   the sun), and the objects, finished (the game draws them first);
2. the dynamic lights, added: a flashlight from the camera and a plasma
   bolt's blue light (the game's lights_render_diffuse);
3. the level's textures, multiplied in (structure_render_diffuse_texture).

The ray-traced lighting takes the light between the stages
(halo_ray_traced_light_stage), and runs after the three.

The scene: a bumpy ground, a back wall, a red wall at the left, an
overhang on a pillar and a crate (the level: in Metal's acceleration
structure), and two marines (objects: not in it), one under the overhang
and one in the open, and a rock under the overhang that is drawn but not
in the collision surfaces. The pictures, as PPM files:

- raytrace_off: without the ray-traced lighting;
- raytrace_after: with it;
- raytrace_occlusion, raytrace_depth: what it uses;
- raytrace_rays: the ray view, what Metal's rays find from the camera;
  raytrace_split: the lighting and the ray view side by side;
- raytrace_probe: the ray probe, the rays of the surface at the center;
- raytrace_undivided: without the stages, as before them: the occlusion
  over all the light, the dynamic lights' too, and the objects' pixels
  found by a short ray (the rock under the overhang taken for one);
- raytrace_sun_0 to _3: the sun from the left, high, from the right and
  low behind the overhang (the marine under it in its shadow).

RT_BENCH=<frames> times the frame with and without the lighting;
RT_DENSE=1 makes the ground about 100,000 triangles (a level's size).

Built and run by port/macos/tests/run_raytrace_test.sh.
*/

#include <SDL3/SDL.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "gl.h"
#include "raytrace_gl.h"

#ifdef HALO_MACOS
/* the host's (port/macos/host/host_metal_rt.m, linked in) */
void host_logf(int priority, const char *format, ...)
{
	va_list arguments;

	(void)priority;
	va_start(arguments, format);
	vprintf(format, arguments);
	va_end(arguments);
	printf("\n");
}
#endif

#define WIDTH 854
#define HEIGHT 480
#define NEAR 0.0625f
#define FAR 1024.0f
#define FIELD_OF_VIEW 1.22f

/* the direction to the sun in the world (right-handed, z up; the camera at
the origin looks along +y) */
static float sun_world[3] = { 0.5f, -0.4f, 0.77f };

unsigned char halo_ray_tracing_sun(float *direction)
{
	float length = sqrtf(sun_world[0] * sun_world[0] + sun_world[1] * sun_world[1] + sun_world[2] * sun_world[2]);

	direction[0] = sun_world[0] / length;
	direction[1] = sun_world[1] / length;
	direction[2] = sun_world[2] / length;
	return 1;
}

void platform_log(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	vprintf(format, arguments);
	va_end(arguments);
	printf("\n");
}

const char *config_string(const char *name)
{
	/* RT_MODE=screen: without Metal's rays */
	if (!strcmp(name, "display.ray_tracing"))
		return getenv("RT_MODE") ? getenv("RT_MODE") : "on";
	/* RT_LIGHTS=game: the game's lights, only their shadows traced */
	if (!strcmp(name, "display.ray_tracing_lights"))
		return getenv("RT_LIGHTS") ? getenv("RT_LIGHTS") : "traced";
	return "";
}

int config_boolean(const char *name)
{
	return !strcmp(name, "display.ray_tracing_objects") ? !getenv("RT_NO_OBJECTS") : 0;
}

double config_real(const char *name)
{
	if (!strcmp(name, "display.ray_tracing_occlusion"))
		return 0.8;
	if (!strcmp(name, "display.ray_tracing_shadows"))
		return 1.0;
	if (!strcmp(name, "display.ray_tracing_reflections"))
		return 0.25;
	return 0.25;
}

static GLuint color_texture, depth_texture, framebuffer;

int xgpu_current_targets(GLuint *color, GLuint *depth, int *width, int *height, int viewport[4])
{
	*color = color_texture;
	*depth = depth_texture;
	*width = WIDTH;
	*height = HEIGHT;
	viewport[0] = 0;
	viewport[1] = 0;
	viewport[2] = WIDTH;
	viewport[3] = HEIGHT;
	return 1;
}

void xgpu_gl_state_invalidate(void)
{
}

void xgpu_gl_bind_device_vertex_array(void)
{
}

/* ---------- the scene, in view space: x right, y up, z into the screen */

static const char scene_vertex[] =
	"#version 300 es\n"
	"precision highp float;\n"
	"in vec3 position;\n"
	"in vec4 color;\n"
	"out vec3 view;\n"
	"out vec3 albedo;\n"
	"flat out float kind;\n"
	"uniform vec4 camera;\n" /* near, far, tan(fov/2), aspect */
	"void main()\n"
	"{\n"
	"	float z = position.z;\n"
	"	vec2 ndc = vec2(position.x / (z * camera.z * camera.w), position.y / (z * camera.z));\n"
	/* the game's depth: 0 at the near plane, 1 at the far one */
	"	float depth = camera.y / (camera.y - camera.x) * (1.0 - camera.x / z);\n"
	/* rows from the top, as the game's targets hold them */
	"	gl_Position = vec4(ndc.x * z, -ndc.y * z, (depth * 2.0 - 1.0) * z, z);\n"
	"	view = position;\n"
	"	albedo = color.rgb;\n"
	"	kind = color.a;\n"
	"}\n";

/* pass 0: the objects, finished, or the level's light; 1: the dynamic
lights, added; 2: the level's textures, multiplied in */
static const char scene_pixel[] =
	"#version 300 es\n"
	"precision highp float;\n"
	"in vec3 view;\n"
	"in vec3 albedo;\n"
	"flat in float kind;\n"
	"uniform int pass;\n"
	"uniform vec4 sun;\n"
	"out vec4 result;\n"
	"void main()\n"
	"{\n"
	"	vec3 N = normalize(cross(dFdx(view), dFdy(view)));\n"
	"	if (dot(N, view) > 0.0) N = -N;\n"
	"	vec3 ambient = vec3(0.30, 0.33, 0.40);\n"
	"	vec3 sunlight = vec3(0.75, 0.70, 0.60) * max(dot(N, sun.xyz), 0.0);\n"
	"	if (pass == 0)\n"
	"	{\n"
	"		vec3 light = ambient + sunlight;\n"
	"		result = vec4(kind > 0.5 ? albedo * light : light, 1.0);\n"
	"	}\n"
	"	else if (pass == 1)\n"
	"	{\n"
	/* the flashlight: a cone from the camera, a little below it */
	"		vec3 from = view - vec3(0.0, -0.2, 0.0);\n"
	"		float distance = length(from);\n"
	"		vec3 axis = normalize(vec3(-3.3, -0.9, 9.0) - vec3(0.0, -0.2, 0.0));\n"
	"		float cone = smoothstep(0.93, 0.975, dot(from / distance, axis));\n"
	"		vec3 flashlight = vec3(1.0, 0.95, 0.8) * 1.4 * cone * max(dot(N, -from / distance), 0.0) /\n"
	"			(1.0 + distance * distance * 0.01);\n"
	/* the plasma bolt's light: blue, near the back left corner */
	"		vec3 to = vec3(-3.0, -0.4, 14.5) - view;\n"
	"		float d = length(to);\n"
	"		vec3 plasma = vec3(0.25, 0.55, 1.6) * max(dot(N, to / d), 0.0) * max(1.0 - d / 4.5, 0.0);\n"
	"		result = vec4(flashlight + plasma, 1.0);\n"
	"	}\n"
	"	else\n"
	"	{\n"
	/* a texture: a check and a little grain */
	"		vec2 cell = floor(vec2(view.x + view.y, view.z + view.y) * 2.0);\n"
	"		float check = mod(cell.x + cell.y, 2.0) * 0.12;\n"
	"		float grain = fract(sin(dot(floor(view.xz * 16.0), vec2(12.9898, 78.233))) * 43758.5453) * 0.08;\n"
	"		result = vec4(albedo * (0.84 + check + grain), 1.0);\n"
	"	}\n"
	"}\n";

struct vertex { float x, y, z, r, g, b, kind; };

static struct vertex *vertices;
/* each vertex's triangle's outward normal (view space), for the level's
triangles' winding */
static float *outwards;
/* the vertices: the level's in the rays' world, then the level's drawn only
(where its rendered surfaces stand out of its collision surfaces), then
the objects' */
static int vertex_count, vertex_capacity, world_vertex_count, structure_vertex_count;

static void add_triangle(const float *a, const float *b, const float *c, const float *albedo, float kind,
	const float *outward)
{
	const float *corners[3] = { a, b, c };
	int index;

	if (vertex_count + 3 > vertex_capacity)
	{
		vertex_capacity = vertex_capacity ? vertex_capacity * 2 : 4096;
		vertices = realloc(vertices, (size_t)vertex_capacity * sizeof(*vertices));
		outwards = realloc(outwards, (size_t)vertex_capacity * 3 * sizeof(float));
	}
	for (index = 0; index < 3; index++)
	{
		struct vertex *v = &vertices[vertex_count];

		v->x = corners[index][0];
		v->y = corners[index][1];
		v->z = corners[index][2];
		v->r = albedo[0];
		v->g = albedo[1];
		v->b = albedo[2];
		v->kind = kind;
		memcpy(&outwards[vertex_count * 3], outward, 3 * sizeof(float));
		vertex_count++;
	}
}

static void add_quad(const float *a, const float *b, const float *c, const float *d, const float *albedo, float kind,
	const float *outward)
{
	add_triangle(a, b, c, albedo, kind, outward);
	add_triangle(a, c, d, albedo, kind, outward);
}

static void add_box(float x0, float y0, float z0, float x1, float y1, float z1, const float *albedo, float kind)
{
	float p[8][3];
	static const float normals[6][3] = { { -1, 0, 0 }, { 1, 0, 0 }, { 0, -1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 } };
	static const int faces[6][4] = { { 0, 2, 6, 4 }, { 1, 5, 7, 3 }, { 0, 4, 5, 1 }, { 2, 3, 7, 6 }, { 0, 1, 3, 2 },
		{ 4, 6, 7, 5 } };
	int corner, face;

	for (corner = 0; corner < 8; corner++)
	{
		p[corner][0] = corner & 1 ? x1 : x0;
		p[corner][1] = corner & 2 ? y1 : y0;
		p[corner][2] = corner & 4 ? z1 : z0;
	}
	for (face = 0; face < 6; face++)
		add_quad(p[faces[face][0]], p[faces[face][1]], p[faces[face][2]], p[faces[face][3]], albedo, kind, normals[face]);
}

static float ground(float x, float z)
{
	return -1.0f + 0.18f * sinf(x * 1.1f) * cosf(z * 0.8f) + 0.08f * sinf(x * 3.1f + z * 2.3f);
}

static void build_scene(int dense)
{
	static const float up[3] = { 0, 1, 0 }, toward[3] = { 0, 0, -1 }, right[3] = { 1, 0, 0 };
	static const float grass[3] = { 0.45f, 0.55f, 0.35f }, stone[3] = { 0.45f, 0.5f, 0.62f }, red[3] = { 0.8f, 0.22f, 0.16f };
	static const float concrete[3] = { 0.7f, 0.68f, 0.62f }, crate[3] = { 0.85f, 0.7f, 0.35f };
	static const float armour[3] = { 0.35f, 0.55f, 0.3f }, orange[3] = { 0.9f, 0.55f, 0.2f };
	int columns = dense ? 200 : 60, rows = dense ? 250 : 75, i, j;
	float x0 = -10, x1 = 10, z0 = 1, z1 = 26;

	vertex_count = 0;
	/* the level: the ground */
	for (j = 0; j < rows; j++)
		for (i = 0; i < columns; i++)
		{
			float xa = x0 + (x1 - x0) * i / columns, xb = x0 + (x1 - x0) * (i + 1) / columns;
			float za = z0 + (z1 - z0) * j / rows, zb = z0 + (z1 - z0) * (j + 1) / rows;
			float a[3] = { xa, ground(xa, za), za }, b[3] = { xb, ground(xb, za), zb - (zb - za) };
			float c[3] = { xb, ground(xb, zb), zb }, d[3] = { xa, ground(xa, zb), zb };

			add_quad(a, b, c, d, grass, 0, up);
		}
	/* the back wall, the red wall at the left */
	{
		float a[3] = { -10, -1.6f, 18 }, b[3] = { 10, -1.6f, 18 }, c[3] = { 10, 6, 18 }, d[3] = { -10, 6, 18 };
		float e[3] = { -4, -1.6f, 3 }, f[3] = { -4, -1.6f, 18 }, g[3] = { -4, 5, 18 }, h[3] = { -4, 5, 3 };

		add_quad(a, b, c, d, stone, 0, toward);
		add_quad(e, f, g, h, red, 0, right);
	}
	/* the overhang on its pillar, and a crate */
	add_box(1, 2.2f, 8, 6, 2.6f, 13, concrete, 0);
	add_box(5.3f, -1.6f, 8, 6, 2.2f, 8.7f, concrete, 0);
	add_box(-1, -1.6f, 5, 0.5f, 0.3f, 6.5f, crate, 0);
	world_vertex_count = vertex_count;
	/* a rock under the overhang, drawn but not in the collision surfaces
	(as the level's rendered rock stands out of them): the level's, so no
	traced sun shadow */
	add_box(2.2f, -1.6f, 11.2f, 3.0f, -0.55f, 12.2f, stone, 0);
	structure_vertex_count = vertex_count;
	/* the objects: a marine under the overhang, one in the open */
	add_box(3.2f, -1.0f, 10.2f, 3.8f, 0.8f, 10.8f, armour, 1);
	add_box(-2.4f, -1.0f, 8.8f, -1.8f, 0.8f, 9.4f, orange, 1);
}

static GLuint program_from(const char *vertex_source, const char *pixel_source)
{
	GLuint vertex = glCreateShader(GL_VERTEX_SHADER), pixel = glCreateShader(GL_FRAGMENT_SHADER);
	GLuint program = glCreateProgram();
	GLint status = 0;
	char log[4096];

	glShaderSource(vertex, 1, &vertex_source, NULL);
	glCompileShader(vertex);
	glShaderSource(pixel, 1, &pixel_source, NULL);
	glCompileShader(pixel);
	glGetShaderiv(pixel, GL_COMPILE_STATUS, &status);
	if (!status)
	{
		glGetShaderInfoLog(pixel, sizeof(log), NULL, log);
		fprintf(stderr, "scene shader: %s\n", log);
	}
	glAttachShader(program, vertex);
	glAttachShader(program, pixel);
	glBindAttribLocation(program, 0, "position");
	glBindAttribLocation(program, 1, "color");
	glLinkProgram(program);
	return program;
}

/* the level's triangles in the world (x, z, y of the view), wound
counterclockwise around their outward normal, as raytrace_world.c has
them */
static float *world_vertices;
static unsigned int *world_indices;

unsigned long halo_ray_tracing_world(const float **world, long *world_vertex_count_out, const unsigned long **indices,
	long *triangle_count)
{
	static unsigned long generation;
	int index;

	if (!generation)
	{
		world_vertices = malloc((size_t)world_vertex_count * 3 * sizeof(float));
		world_indices = malloc((size_t)world_vertex_count * sizeof(unsigned int));
		for (index = 0; index < world_vertex_count; index++)
		{
			world_vertices[index * 3 + 0] = vertices[index].x;
			world_vertices[index * 3 + 1] = vertices[index].z;
			world_vertices[index * 3 + 2] = vertices[index].y;
			world_indices[index] = (unsigned int)index;
		}
		for (index = 0; index + 2 < world_vertex_count; index += 3)
		{
			float *a = &world_vertices[index * 3], *b = a + 3, *c = a + 6;
			float u[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, v[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
			float n[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
			const float *o = &outwards[index * 3];

			if (n[0] * o[0] + n[1] * o[2] + n[2] * o[1] < 0.0f)
			{
				unsigned int swap = world_indices[index + 1];

				world_indices[index + 1] = world_indices[index + 2];
				world_indices[index + 2] = swap;
			}
		}
		generation = 1;
	}
	*world = world_vertices;
	*world_vertex_count_out = world_vertex_count;
	*indices = (const unsigned long *)world_indices;
	*triangle_count = world_vertex_count / 3;
	return generation;
}

/* the objects as triangles for the rays: the marines, each its box (as a
unit's collision model is its shape), wound counterclockwise seen from
outside; the one in the open as the player's body, whose shadow only the
rays draw */
long halo_ray_tracing_objects(float *triangles, unsigned char *groups, float *cutouts, long maximum,
	const float *camera, float *player_sphere, long shapes)
{
	(void)cutouts;
	/* view space: x, y (up), z (forward); the world's x, y, z are the
	view's x, z, y */
	static const float boxes[2][6] = { { 3.2f, -1.0f, 10.2f, 3.8f, 0.8f, 10.8f }, { -2.4f, -1.0f, 8.8f, -1.8f, 0.8f, 9.4f } };
	static const int faces[6][4] = { { 0, 2, 6, 4 }, { 1, 5, 7, 3 }, { 0, 4, 5, 1 }, { 2, 3, 7, 6 }, { 0, 1, 3, 2 },
		{ 4, 6, 7, 5 } };
	long count = 0, index;

	(void)camera;
	(void)shapes;
	/* the player's body: the marine in the open (world x, y, z) */
	player_sphere[0] = -2.1f;
	player_sphere[1] = 9.1f;
	player_sphere[2] = -0.1f;
	player_sphere[3] = 1.0f;
	if (getenv("RT_NO_OBJECTS"))
		return 0;
	for (index = 0; index < 2; index++)
	{
		const float *b = boxes[index];
		float corners[8][3], center[3];
		int corner, face, half;

		for (corner = 0; corner < 8; corner++)
		{
			corners[corner][0] = corner & 1 ? b[3] : b[0];
			corners[corner][1] = corner & 4 ? b[5] : b[2];
			corners[corner][2] = corner & 2 ? b[4] : b[1];
		}
		center[0] = (b[0] + b[3]) * 0.5f;
		center[1] = (b[2] + b[5]) * 0.5f;
		center[2] = (b[1] + b[4]) * 0.5f;
		for (face = 0; face < 6; face++)
		{
			for (half = 0; half < 2 && count < maximum; half++)
			{
				const float *p = corners[faces[face][0]], *q = corners[faces[face][half ? 2 : 1]];
				const float *r = corners[faces[face][half ? 3 : 2]];
				float u[3] = { q[0] - p[0], q[1] - p[1], q[2] - p[2] }, v[3] = { r[0] - p[0], r[1] - p[1], r[2] - p[2] };
				float n[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
				float out = n[0] * (p[0] - center[0]) + n[1] * (p[1] - center[1]) + n[2] * (p[2] - center[2]);
				float *t = triangles + count * 9;

				memcpy(t, p, sizeof(float) * 3);
				memcpy(t + 3, out < 0.0f ? r : q, sizeof(float) * 3);
				memcpy(t + 6, out < 0.0f ? q : r, sizeof(float) * 3);
				groups[count++] = index == 1 ? 4 : (1 << 3 | 2);
			}
		}
	}
	return count;
}

/* the dynamic lights, as the scene's second pass draws them (world: the
view's x, z, y): the flashlight from under the camera, and the plasma
bolt's light */
long halo_ray_tracing_lights(float *lights, long maximum, long all)
{
	float axis[3] = { -3.3f, 9.0f, -0.7f };
	float length = sqrtf(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
	const float scene[3][12] = {
		{ 0.0f, 0.0f, -0.2f, 25.0f, axis[0] / length, axis[1] / length, axis[2] / length, 0.93f, 0.0f, 1.0f, 0.95f, 0.85f },
		{ -3.0f, 14.5f, -0.4f, 4.5f, 0.0f, 0.0f, 1.0f, -2.0f, 0.0f, 0.3f, 0.6f, 1.0f },
		/* (a light the game draws on the objects only - Guilty Spark's) */
		{ 2.5f, 6.0f, 1.2f, 5.0f, 0.0f, 0.0f, 1.0f, -2.0f, 0.5f, 0.4f, 0.8f, 1.0f } };
	long count = getenv("RT_NO_LIGHTS") ? 0 : all ? 3 : 2;

	if (count > maximum)
		count = maximum;
	memcpy(lights, scene, (size_t)count * 12 * sizeof(float));
	return count;
}

/* the drawn level, its materials and pages, and the sky: none here (the
collision level stands in) */
unsigned long halo_ray_tracing_level(const float **vertices, const float **texcoords, const float **base_texcoords,
	long *vertex_count, const unsigned long **indices, const unsigned long **triangle_materials, long *triangle_count,
	long *cutout_start)
{
	(void)base_texcoords;
	(void)cutout_start;
	(void)vertices;
	(void)texcoords;
	(void)vertex_count;
	(void)indices;
	(void)triangle_materials;
	(void)triangle_count;
	return 0;
}

unsigned char halo_ray_tracing_mask(long *index, const unsigned char **alpha, long *width, long *height,
	unsigned long *generation)
{
	(void)index;
	(void)alpha;
	(void)width;
	(void)height;
	*generation = 0;
	return 0;
}

unsigned char halo_ray_tracing_level_materials(const float **materials, long *count)
{
	(void)materials;
	(void)count;
	return 0;
}

unsigned char halo_ray_tracing_level_page(long *page, const unsigned char **pixels, long *width, long *height)
{
	(void)page;
	(void)pixels;
	(void)width;
	(void)height;
	return 0;
}

void halo_ray_tracing_level_pages(long *done, long *total)
{
	*done = *total = 0;
}

unsigned char halo_ray_tracing_sky(float *sky)
{
	(void)sky;
	return 0;
}

/* the emitters: a needle's pink glow by the crate (world: the view's x, z, y) */
long halo_ray_tracing_emitters(float *emitters, long maximum, const float *camera)
{
	static const float needle[8] = { -1.6f, 4.6f, -0.45f, 1.5f, 1.0f, 0.3f, 0.8f, 1.2f };

	(void)camera;
	if (maximum < 1 || getenv("RT_NO_EMITTERS"))
		return 0;
	memcpy(emitters, needle, sizeof(needle));
	return 1;
}

static GLuint program, buffer, array;

static void draw_pass(int pass)
{
	float camera[4] = { NEAR, FAR, tanf(FIELD_OF_VIEW * 0.5f), (float)WIDTH / HEIGHT };
	float length = sqrtf(sun_world[0] * sun_world[0] + sun_world[1] * sun_world[1] + sun_world[2] * sun_world[2]);
	float sun_view[4] = { sun_world[0] / length, sun_world[2] / length, sun_world[1] / length, 0.0f };

	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glViewport(0, 0, WIDTH, HEIGHT);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_STENCIL_TEST);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glEnable(GL_DEPTH_TEST);
	glUseProgram(program);
	glUniform4fv(glGetUniformLocation(program, "camera"), 1, camera);
	glUniform4fv(glGetUniformLocation(program, "sun"), 1, sun_view);
	glUniform1i(glGetUniformLocation(program, "pass"), pass);
	glBindVertexArray(array);
	if (pass == 0 || pass == 3)
	{
		glUniform1i(glGetUniformLocation(program, "pass"), 0);
		glDisable(GL_BLEND);
		glDepthMask(GL_TRUE);
		glDepthFunc(GL_LEQUAL);
		if (pass == 0)
		{
			/* the objects first, as the game draws them */
			glClearColor(0.5f, 0.7f, 0.9f, 1.0f);
			glClearDepthf(1.0f);
			glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
			glDrawArrays(GL_TRIANGLES, structure_vertex_count, vertex_count - structure_vertex_count);
		}
		else
		{
			glDrawArrays(GL_TRIANGLES, 0, structure_vertex_count);
		}
		return;
	}
	/* the level's surfaces only, where they are seen */
	glEnable(GL_BLEND);
	glDepthMask(GL_FALSE);
	glDepthFunc(GL_EQUAL);
	if (pass == 1)
		glBlendFunc(GL_ONE, GL_ONE);
	else
		glBlendFunc(GL_DST_COLOR, GL_ZERO);
	glDrawArrays(GL_TRIANGLES, 0, structure_vertex_count);
	glDisable(GL_BLEND);
	glDepthMask(GL_TRUE);
	glDepthFunc(GL_LEQUAL);
}

static const float camera_position[3] = { 0, 0, 0 }, camera_forward[3] = { 0, 1, 0 }, camera_up[3] = { 0, 0, 1 };

/* a frame, as the game draws it; lighting: 0 none, 1 the ray-traced
lighting, 2 it without the stages (the dynamic lights darkened too, the
objects' pixels found by probing) */
static void draw_frame(int lighting)
{
	if (!program)
	{
		program = program_from(scene_vertex, scene_pixel);
		glGenVertexArrays(1, &array);
		glBindVertexArray(array);
		glGenBuffers(1, &buffer);
		glBindBuffer(GL_ARRAY_BUFFER, buffer);
		glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)vertex_count * sizeof(struct vertex), vertices, GL_STATIC_DRAW);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(struct vertex), (void *)0);
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(struct vertex), (void *)12);
	}
	draw_pass(0);
	if (lighting == 1)
		halo_ray_traced_light_stage(2);
	draw_pass(3);
	if (lighting == 1)
		halo_ray_traced_light_stage(0);
	draw_pass(1);
	if (lighting == 1)
		halo_ray_traced_light_stage(1);
	draw_pass(2);
	if (lighting)
		halo_ray_traced_lighting(NEAR, FAR, FIELD_OF_VIEW, camera_position, camera_forward, camera_up);
}

static void save(const char *path)
{
	static unsigned char pixels[WIDTH * HEIGHT * 4];
	FILE *file = fopen(path, "wb");
	int y, x;

	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glReadPixels(0, 0, WIDTH, HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	fprintf(file, "P6\n%d %d\n255\n", WIDTH, HEIGHT);
	/* the targets' rows run from the top */
	for (y = 0; y < HEIGHT; y++)
		for (x = 0; x < WIDTH; x++)
			fwrite(&pixels[(y * WIDTH + x) * 4], 1, 3, file);
	fclose(file);
	printf("wrote %s\n", path);
}

static double bench(int lighting, int frames)
{
	uint64_t start;
	int frame;

	draw_frame(lighting);
	glFinish();
	start = SDL_GetTicksNS();
	for (frame = 0; frame < frames; frame++)
		draw_frame(lighting);
	glFinish();
	return (double)(SDL_GetTicksNS() - start) / 1e6 / frames;
}

int main(int argc, char **argv)
{
	SDL_Window *window;
	SDL_GLContext context;
	char path[1024];
	static const float suns[4][3] = { { -0.8f, 0.1f, 0.55f }, { 0.1f, 0.2f, 1.0f }, { 0.8f, -0.1f, 0.5f },
		{ -0.05f, 0.9f, 0.45f } };
	int index;

	if (argc < 2)
	{
		fprintf(stderr, "usage: raytrace_test <angle folder>\n");
		return 2;
	}
	snprintf(path, sizeof(path), "%s/libGLESv2.dylib", argv[1]);
	SDL_SetHint(SDL_HINT_OPENGL_LIBRARY, path);
	snprintf(path, sizeof(path), "%s/libEGL.dylib", argv[1]);
	SDL_SetHint(SDL_HINT_EGL_LIBRARY, path);
	setenv("ANGLE_DEFAULT_PLATFORM", "metal", 0);
	if (!SDL_Init(SDL_INIT_VIDEO))
	{
		fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
		return 1;
	}
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
	window = SDL_CreateWindow("raytrace test", 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
	context = window ? SDL_GL_CreateContext(window) : NULL;
	if (!context || !gl_functions_load())
	{
		fprintf(stderr, "no OpenGL ES context: %s\n", SDL_GetError());
		return 1;
	}
	printf("%s\n", (const char *)glGetString(GL_RENDERER));
	glGenTextures(1, &color_texture);
	glBindTexture(GL_TEXTURE_2D, color_texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, WIDTH, HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenTextures(1, &depth_texture);
	glBindTexture(GL_TEXTURE_2D, depth_texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, WIDTH, HEIGHT, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
	glGenFramebuffers(1, &framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_texture, 0);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, depth_texture, 0);

	build_scene(getenv("RT_DENSE") && atoi(getenv("RT_DENSE")));
	printf("the scene: %d triangles in the level's rays, %d drawn only, %d in the objects\n", world_vertex_count / 3,
		(structure_vertex_count - world_vertex_count) / 3, (vertex_count - structure_vertex_count) / 3);

	if (getenv("RT_BENCH"))
	{
		int frames = atoi(getenv("RT_BENCH"));

		if (frames < 1)
			frames = 100;
		halo_ray_tracing_debug_mode(1);
		printf("the frame without the lighting: %.3f ms\n", bench(0, frames));
		printf("the frame with the lighting: %.3f ms\n", bench(1, frames));
		SDL_Quit();
		return 0;
	}

	draw_frame(0);
	save("raytrace_off.ppm");
	halo_ray_tracing_debug_mode(1);
	draw_frame(1);
	save("raytrace_after.ppm");
	draw_frame(2);
	save("raytrace_undivided.ppm");
	halo_ray_tracing_debug_mode(2);
	draw_frame(1);
	save("raytrace_occlusion.ppm");
	halo_ray_tracing_debug_mode(3);
	draw_frame(1);
	save("raytrace_depth.ppm");
	/* what Metal's rays find from the camera, and beside the lighting */
	halo_ray_tracing_debug_mode(4);
	draw_frame(1);
	save("raytrace_rays.ppm");
	halo_ray_tracing_debug_mode(5);
	draw_frame(1);
	save("raytrace_split.ppm");
	/* the ray probe: the rays of the surface at the center, drawn (a frame
	to trace them, one to draw them) */
	halo_ray_tracing_debug_mode(1);
	halo_ray_tracing_probe_next();
	draw_frame(1);
	glFinish();
	draw_frame(1);
	save("raytrace_probe.ppm");
	halo_ray_tracing_probe_next();
	halo_ray_tracing_probe_next();
	halo_ray_tracing_debug_mode(1);
	for (index = 0; index < 4; index++)
	{
		memcpy(sun_world, suns[index], sizeof(sun_world));
		draw_frame(1);
		snprintf(path, sizeof(path), "raytrace_sun_%d.ppm", index);
		save(path);
	}
	printf("GL error %#x\n", glGetError());
	SDL_Quit();
	return 0;
}
