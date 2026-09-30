/*
HOST_METAL_RT.M

World-space ray tracing for the macOS port's lighting
(port/linux/src/raytrace_gl.c), with Metal's ray tracing: in compute on
M1 and M2, in the GPU's ray tracing hardware on M3 and later.

- The level: port/linux/game/raytrace_world.c gives the active BSP's
  collision surfaces as triangles (host_rt_set_world), and they become a
  Metal acceleration structure, rebuilt when the BSP changes.
- The frame: the guest draws each pixel's linear depth and normal into a
  texture it shares with Metal (host_rt_texture: a Metal texture that
  ANGLE, which draws the game on the same Metal device, takes as a GL
  texture through EGL_ANGLE_metal_texture_client_buffer).
- The rays (host_rt_trace): from each pixel's point in the world, four rays
  over the hemisphere around its normal, which find the level's geometry
  near it wherever it is, on screen or not (ambient occlusion), and one
  along the view's reflection, whose hit is projected back onto the screen
  for its colour if the camera sees it there. The results go to a second
  shared texture, which the guest composites.

GL and Metal take turns on the GPU, through a Metal shared event
(EGL_ANGLE_metal_shared_event_sync): GL signals it when the depth and
normals are drawn, the rays wait for that and signal it when done, and GL
waits for that before it reads the results. The CPU waits for neither, so
it prepares the next frame while the GPU traces. Without the extension,
glFinish before the rays and the command buffer completed before GL
reads the results.
*/

#include "host.h"

#import <Metal/Metal.h>
#include <SDL3/SDL.h>
#include <dlfcn.h>
#include <math.h>
#include <string.h>

/* EGL and GL, as ANGLE has them (SDL loaded both) */
typedef void *EGLDisplay_;
typedef void *EGLImage_;
typedef intptr_t EGLAttrib_;
typedef int EGLint_;
#define EGL_NO_CONTEXT_ ((void *)0)
#define EGL_DEVICE_EXT_ 0x322C
#define EGL_METAL_DEVICE_ANGLE_ 0x34A6
#define EGL_METAL_TEXTURE_ANGLE_ 0x34A7
#define EGL_NONE_ 0x3038
#define EGL_EXTENSIONS_ 0x3055
#define EGL_SYNC_CONDITION_ 0x30F8
#define EGL_SYNC_METAL_SHARED_EVENT_ANGLE_ 0x34D8
#define EGL_SYNC_METAL_SHARED_EVENT_OBJECT_ANGLE_ 0x34D9
#define EGL_SYNC_METAL_SHARED_EVENT_SIGNAL_VALUE_LO_ANGLE_ 0x34DA
#define EGL_SYNC_METAL_SHARED_EVENT_SIGNAL_VALUE_HI_ANGLE_ 0x34DB
#define EGL_SYNC_METAL_SHARED_EVENT_SIGNALED_ANGLE_ 0x34DC
#define GL_TEXTURE_2D_ 0x0DE1
#define GL_TEXTURE_MIN_FILTER_ 0x2801
#define GL_TEXTURE_MAG_FILTER_ 0x2800
#define GL_NEAREST_ 0x2600

#define HOST_RT_MAXIMUM_OBJECTS 511
#define HOST_RT_MAXIMUM_LIGHTS 16
#define HOST_RT_MAXIMUM_EMITTERS 16
/* the objects, each its own mesh (its bounds its own, so that a ray far
from them all walks none), and its triangles at most; and all of theirs */
#define HOST_RT_GROUPS 32
#define HOST_RT_GROUP_TRIANGLES 8192
#define HOST_RT_OBJECT_TRIANGLES 65536

static struct
{
	int checked, available;
	id<MTLDevice> device;
	id<MTLCommandQueue> queue;
	id<MTLComputePipelineState> pipeline;
	id<MTLAccelerationStructure> world;
	unsigned int world_generation;
	/* the drawn level (host_rt_set_level): its structure, its triangles'
	indices, its vertices' lightmap coordinates and each triangle's material;
	the materials (host_rt_set_level_materials: 2 float4 each); the lightmap
	pages, packed into one texture (host_rt_set_level_page), and where each
	is in it (a float4 each: its corner and size in the texture's units);
	whether the rays use it this frame (the camera's 46) */
	id<MTLAccelerationStructure> drawn;
	unsigned int drawn_generation;
	int use_drawn;
	id<MTLBuffer> drawn_indices, drawn_texcoords, drawn_triangle_materials, drawn_materials, drawn_pages;
	/* the drawn level's glowing triangles, for the rays to them (3 float4
	each: the corners; their w, the chance of the triangle, the sum of the
	chances to it, its material) and how many; the vertices, kept for it */
	id<MTLBuffer> drawn_vertices, glowing;
	uint32_t glowing_count;
	/* the light grid: the level's box in cells, each with the glowing
	triangles that light it most (their light over their distance squared,
	the 32 most), built apart as the glowing ones change (light_grid_build),
	and the glowing triangles it was built from */
	id<MTLBuffer> grid_info, grid_cells, grid_entries;
	__unsafe_unretained id<MTLBuffer> grid_glowing;
	/* the cutouts (alpha-tested): the drawn level's base map coordinates and
	its first alpha-tested triangle; the masks, packed into one texture, and
	where each is in it; the objects' cutouts this frame (2 float4 each:
	corners' coordinates, mask), in turn, and each instance's first */
	id<MTLBuffer> drawn_base_texcoords;
	uint32_t drawn_cutout_start;
	id<MTLTexture> mask_atlas;
	id<MTLBuffer> mask_rects;
	unsigned long mask_generation;
	int mask_x, mask_y, mask_row;
	float *object_cutout_input;
	id<MTLBuffer> object_cutouts[3], instance_offsets[3], cut_vertices[3][HOST_RT_GROUPS];
	int scene_ring;
	/* the light probes: their pipeline, the points asked for this frame,
	the results' buffers in turn, and the last results read back (7 floats
	each: the point, the light, how much from one way; then the way in
	probe_directions) */
	id<MTLComputePipelineState> probe_pipeline;
	float probe_points[64 * 4];
	uint32_t probe_count;
	id<MTLBuffer> probe_in[3], probe_out[3];
	int probe_ring;
	/* the exposure: the level's baked light over its traced light, where
	both are known (a few of the level's pixels, summed by the rays, read
	back when they are done), followed slowly; the traced light is taken
	times it, so the rays' light is as bright, overall, as the level's
	artists lit it */
	id<MTLBuffer> exposure_sums[3];
	int exposure_ring;
	float exposure;
	/* the white balance: each colour's multiplier (the lightmaps' tint) */
	float balance[3];
	/* the objects' shapes' last build's time on the GPU */
	double build_ms;
	float probe_results[64 * 10];
	int probe_result_count;
	id<MTLTexture> atlas;
	int atlas_x, atlas_y, atlas_row;
	/* the traced light, accumulated over frames: the last frame's and this
	one's, in turn */
	id<MTLTexture> history[2];
	int history_index;
	/* the objects (host_rt_set_objects): a mesh for each, and the scene of
	the level and them. The buffers the frame writes are in rings of three:
	the GPU may still read the last ones. A mesh whose triangles are the
	last frame's is kept; one with as many, moved (a character's, animated),
	refitted (its boxes moved to them, a fraction of a build); built anew
	when its triangles are others, and every 60th frame */
	id<MTLAccelerationStructure> bodies[HOST_RT_GROUPS], scene;
	int body_counts[HOST_RT_GROUPS][2], body_age[HOST_RT_GROUPS];
	/* all the frame's object triangles for the kernel, 10 floats each (the
	corners; the colour's bits), each instance's opaque ones then its
	cutouts, and where each instance's start (uint4: opaque, cutouts,
	whether it has opaque ones) */
	id<MTLBuffer> object_tris[3], instance_tris[3];
	uint32_t tri_base[HOST_RT_GROUPS][2];
	NSArray<id<MTLAccelerationStructure>> *scene_structures;
	float spheres[HOST_RT_GROUPS + 1][4];
	/* the ray probe's segments (the kernel writes them) */
	id<MTLBuffer> probe;
	unsigned int sphere_count;
	id<MTLBuffer> body_vertices[3][HOST_RT_GROUPS], body_scratch[HOST_RT_GROUPS], instance_buffers[3], scene_scratch;
	int instance_ring;
	/* the objects' triangles in the world this frame, and each one's group
	(its object above, its kind - 2 an object, 4 the player's body - below) */
	float *object_triangles;
	unsigned char *object_groups;
	int object_count, objects_two_sided;
	id<MTLTexture> textures[4];
	unsigned int gl_textures[4];
	EGLImage_ images[4];
	int widths[4], heights[4];
	/* the lights (host_rt_set_lights): 12 floats each */
	float lights[HOST_RT_MAXIMUM_LIGHTS * 12];
	unsigned int light_count;
	/* the emitters (host_rt_set_emitters): 8 floats each */
	float emitters[HOST_RT_MAXIMUM_EMITTERS * 8];
	unsigned int emitter_count;
	/* the governor: the last trace's time on the GPU (its completion
	handler's), how far the lights and emitters are cut back (each step
	halves them), the calm frames since the last step, and the frames in a
	row far over, after which the rays stop - a GPU busy for long enough
	freezes the whole machine's display */
	volatile double gpu_ms;
	int shed, calm, overloaded;
	EGLDisplay_ display;
	EGLDisplay_ (*eglGetCurrentDisplay)(void);
	EGLImage_ (*eglCreateImageKHR)(EGLDisplay_, void *, unsigned int, void *, const EGLint_ *);
	unsigned int (*eglDestroyImageKHR)(EGLDisplay_, EGLImage_);
	unsigned int (*eglQueryDisplayAttribEXT)(EGLDisplay_, EGLint_, EGLAttrib_ *);
	unsigned int (*eglQueryDeviceAttribEXT)(void *, EGLint_, EGLAttrib_ *);
	void (*glGenTextures)(int, unsigned int *);
	void (*glDeleteTextures)(int, const unsigned int *);
	void (*glBindTexture)(unsigned int, unsigned int);
	void (*glTexParameteri)(unsigned int, unsigned int, int);
	void (*glEGLImageTargetTexture2DOES)(unsigned int, void *);
	void (*glFinish)(void);
	void (*glFlush)(void);
	/* the GPU-side turns: the event and its last value */
	id<MTLSharedEvent> event;
	uint64_t event_value;
	const char *(*eglQueryString)(EGLDisplay_, EGLint_);
	void *(*eglCreateSync)(EGLDisplay_, unsigned int, const EGLAttrib_ *);
	unsigned int (*eglDestroySync)(EGLDisplay_, void *);
	unsigned int (*eglWaitSync)(EGLDisplay_, void *, EGLint_);
} rt;

static NSString *const kernel_source = @
	"#include <metal_stdlib>\n"
	"#include <metal_raytracing>\n"
	"using namespace metal;\n"
	"using namespace raytracing;\n"
	/* c: position 0-2, forward 3-5, up 6-8, right 9-11, near 12, far 13,
	   tan of half the vertical field of view 14, aspect 15, viewport 16-19,
	   frame 20, occlusion radius 21, reflection distance 22, whether the
	   objects' pixels are known 23 (then their depth is negative), the
	   direction to the sun 24-26 and whether there is one 27, the player's
	   body's bounding sphere 28-31 (radius 0: none), the ray view 32 (0 off,
	   1 all the screen, 2 its right half), the ray probe 33 */
	/* the ray probe: a ray of the probe pixel, as a segment (from, kind; to,
	   whether it hit) after the count in probe[0] */
	"static void probe_segment(device float4 *probe, thread uint &n, float3 a, float3 b, float kind, bool hit)\n"
	"{\n"
	"	if (n >= 15u) return;\n"
	"	probe[1u + n * 2u] = float4(a, kind);\n"
	"	probe[2u + n * 2u] = float4(b, hit ? 1.0 : 0.0);\n"
	"	n++;\n"
	"	probe[0] = float4(float(n), 0.0, 0.0, 0.0);\n"
	"}\n"
	"constexpr sampler linear_clamp(filter::linear, address::clamp_to_edge);\n"
	/* random numbers: the PCG hash, a new one from each */
	"static uint pcg(uint v)\n"
	"{\n"
	"	uint state = v * 747796405u + 2891336453u;\n"
	"	uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;\n"
	"	return (word >> 22u) ^ word;\n"
	"}\n"
	"static float random01(thread uint &seed)\n"
	"{\n"
	"	seed = pcg(seed);\n"
	"	return float(seed >> 8) / 16777216.0;\n"
	"}\n"
	/* and spread evenly: a pixel's nth sample the nth point of the R2
	   sequence (each next one where the last left the most room), shifted
	   by the pixel's interleaved gradient noise (neighbours shifted far
	   apart) - over its samples a pixel covers the directions evenly, and
	   what noise is left is fine, not in clumps, for the denoiser */
	"static float2 spread01(uint2 pixel, float n, float salt)\n"
	"{\n"
	"	float2 p = float2(pixel) + salt * float2(5.588238, 3.1415926);\n"
	"	float ign = fract(52.9829189 * fract(dot(p, float2(0.06711056, 0.00583715))));\n"
	"	return fract(float2(ign, fract(ign * 1.6180339887 + 0.5)) + n * float2(0.7548776662, 0.5698402910));\n"
	"}\n"
	/* a glowing triangle to trace toward from P: four times in five one of
	   its cell's (the light grid's: those that light it most), else any, by
	   its light; and the chance it had, both ways */
	"struct glow_choice { uint index; float probability; };\n"
	"#define GRID_PARAMS constant float4 *grid_info, device const uint2 *grid_cells, device const uint2 *grid_entries\n"
	"#define GRID_ARGS grid_info, grid_cells, grid_entries\n"
	"static glow_choice glow_pick(float3 P, float u, device const float4 *glowing, uint glowing_count, GRID_PARAMS)\n"
	"{\n"
	"	uint offset = 0u, count = 0u;\n"
	"	if (grid_info[1].w > 0.5)\n"
	"	{\n"
	"		int3 q = int3(floor((P - grid_info[0].xyz) / grid_info[0].w));\n"
	"		int3 dims = int3(grid_info[1].xyz);\n"
	"		if (all(q >= 0) && all(q < dims))\n"
	"		{\n"
	"			uint2 cell = grid_cells[(uint(q.z) * uint(dims.y) + uint(q.y)) * uint(dims.x) + uint(q.x)];\n"
	"			offset = cell.x;\n"
	"			count = cell.y;\n"
	"		}\n"
	"	}\n"
	"	float share = count > 0u ? 0.8 : 0.0;\n"
	"	glow_choice g;\n"
	"	if (u < share)\n"
	"	{\n"
	"		float v = u / share;\n"
	"		uint lo = 0u, hi = count - 1u;\n"
	"		while (lo < hi) { uint mid = (lo + hi) / 2u; if (as_type<float>(grid_entries[offset + mid].y) < v) lo = mid + 1u; else hi = mid; }\n"
	"		g.index = min(grid_entries[offset + lo].x, glowing_count - 1u);\n"
	"	}\n"
	"	else\n"
	"	{\n"
	"		float v = share > 0.0 ? (u - share) / (1.0 - share) : u;\n"
	"		uint lo = 0u, hi = glowing_count - 1u;\n"
	"		while (lo < hi) { uint mid = (lo + hi) / 2u; if (glowing[mid * 3u + 1u].w < v) lo = mid + 1u; else hi = mid; }\n"
	"		g.index = lo;\n"
	"	}\n"
	"	float in_cell = 0.0;\n"
	"	if (count > 0u)\n"
	"	{\n"
	"		uint lo = 0u, hi = count - 1u;\n"
	"		while (lo < hi) { uint mid = (lo + hi) / 2u; if (grid_entries[offset + mid].x < g.index) lo = mid + 1u; else hi = mid; }\n"
	"		if (grid_entries[offset + lo].x == g.index)\n"
	"			in_cell = as_type<float>(grid_entries[offset + lo].y) - (lo > 0u ? as_type<float>(grid_entries[offset + lo - 1u].y) : 0.0);\n"
	"	}\n"
	"	g.probability = share * in_cell + (1.0 - share) * glowing[g.index * 3u].w;\n"
	"	return g;\n"
	"}\n"
	/* the cutouts (alpha-tested: foliage, fences): a candidate hit on one is
	solid where its mask (its texture's alpha) is - the level's (instance 0;
	its primitives from c[83]) by its material and base map coordinates, an
	object's by the frame's cutouts, from its instance's first */
	"constexpr sampler nearest_clamp(filter::nearest, address::clamp_to_edge);\n"
	"#define CUT_PARAMS uint cutout_start, device const uint *indices, device const float2 *base_texcoords, \\\n"
	"	device const uint *triangle_materials, device const float4 *materials, device const float4 *object_cutouts, \\\n"
	"	device const uint *instance_offsets, device const float4 *mask_rects, texture2d<float, access::sample> masks, \\\n"
	"	uint cutouts_ready\n"
	"#define CUT_ARGS uint(c[83]), indices, base_texcoords, triangle_materials, materials, object_cutouts, \\\n"
	"	instance_offsets, mask_rects, masks, cutouts_ready\n"
	"static bool cutout_solid(uint instance, uint primitive, float2 b, CUT_PARAMS)\n"
	"{\n"
	"	if (cutouts_ready == 0u) return true;\n"
	"	float2 uv;\n"
	"	int mask;\n"
	"	if (instance == 0u)\n"
	"	{\n"
	"		uint triangle = cutout_start + primitive;\n"
	"		uint v = uint(materials[triangle_materials[triangle] * 2u].w);\n"
	"		if (v < 16u) return true;\n"
	"		mask = int(v >> 4) - 1;\n"
	"		uint base = triangle * 3u;\n"
	"		uv = base_texcoords[indices[base]] * (1.0 - b.x - b.y) + base_texcoords[indices[base + 1u]] * b.x +\n"
	"			base_texcoords[indices[base + 2u]] * b.y;\n"
	"	}\n"
	"	else\n"
	"	{\n"
	"		uint at = (instance_offsets[instance] + primitive) * 2u;\n"
	"		float4 a = object_cutouts[at], z = object_cutouts[at + 1u];\n"
	"		uv = a.xy * (1.0 - b.x - b.y) + a.zw * b.x + z.xy * b.y;\n"
	"		mask = int(z.z);\n"
	"	}\n"
	"	if (mask < 0 || mask >= 256) return true;\n"
	"	float4 rect = mask_rects[mask];\n"
	"	if (rect.z <= 0.0) return true;\n"
	"	float2 inset = float2(0.5 / 2048.0);\n"
	"	return masks.sample(nearest_clamp, rect.xy + clamp(fract(uv) * rect.zw, inset, rect.zw - inset), metal::level(0.0)).r >= 0.5;\n"
	"}\n"
	/* whether anything solid is on the ray (from both sides) */
	"static bool blocked(ray r, instance_acceleration_structure world, uint mask_bits, CUT_PARAMS)\n"
	"{\n"
	"	intersection_params params;\n"
	"	params.accept_any_intersection(true);\n"
	"	params.assume_geometry_type(geometry_type::triangle);\n"
	/* (without the masks, all solid: no candidates to look at) */
	"	if (cutouts_ready == 0u) params.force_opacity(forced_opacity::opaque);\n"
	"	intersection_query<triangle_data, instancing> q(r, world, mask_bits, params);\n"
	"	while (q.next())\n"
	"	{\n"
	"		if (q.get_candidate_intersection_type() == intersection_type::triangle &&\n"
	"			cutout_solid(q.get_candidate_instance_id(), q.get_candidate_primitive_id(),\n"
	"				q.get_candidate_triangle_barycentric_coord(), cutout_start, indices, base_texcoords, triangle_materials,\n"
	"				materials, object_cutouts, instance_offsets, mask_rects, masks, cutouts_ready))\n"
	"			q.commit_triangle_intersection();\n"
	"	}\n"
	"	return q.get_committed_intersection_type() != intersection_type::none;\n"
	"}\n"
	/* the nearest solid thing on the ray (the level's primitive: its
	triangle's, cutouts after the opaque) */
	"struct hit_info\n"
	"{\n"
	"	intersection_type type;\n"
	"	float distance;\n"
	"	uint instance_id, primitive_id, geometry_id;\n"
	"	float2 triangle_barycentric_coord;\n"
	"	bool triangle_front_facing;\n"
	"};\n"
	"static hit_info closest_hit(ray r, instance_acceleration_structure world, uint mask_bits, CUT_PARAMS)\n"
	"{\n"
	"	intersection_params params;\n"
	"	params.assume_geometry_type(geometry_type::triangle);\n"
	"	params.set_triangle_front_facing_winding(winding::clockwise);\n"
	"	if (cutouts_ready == 0u) params.force_opacity(forced_opacity::opaque);\n"
	"	intersection_query<triangle_data, instancing> q(r, world, mask_bits, params);\n"
	"	while (q.next())\n"
	"	{\n"
	"		if (q.get_candidate_intersection_type() == intersection_type::triangle &&\n"
	"			cutout_solid(q.get_candidate_instance_id(), q.get_candidate_primitive_id(),\n"
	"				q.get_candidate_triangle_barycentric_coord(), cutout_start, indices, base_texcoords, triangle_materials,\n"
	"				materials, object_cutouts, instance_offsets, mask_rects, masks, cutouts_ready))\n"
	"			q.commit_triangle_intersection();\n"
	"	}\n"
	"	hit_info h;\n"
	"	h.type = q.get_committed_intersection_type();\n"
	"	h.distance = 0.0;\n"
	"	h.instance_id = 0u;\n"
	"	h.primitive_id = 0u;\n"
	"	h.geometry_id = 0u;\n"
	"	h.triangle_barycentric_coord = float2(0.0);\n"
	"	h.triangle_front_facing = true;\n"
	"	if (h.type != intersection_type::none)\n"
	"	{\n"
	"		h.distance = q.get_committed_distance();\n"
	"		h.instance_id = q.get_committed_instance_id();\n"
	"		h.geometry_id = q.get_committed_geometry_id();\n"
	"		h.primitive_id = q.get_committed_primitive_id() +\n"
	"			(h.instance_id == 0u && q.get_committed_geometry_id() == 1u ? cutout_start : 0u);\n"
	"		h.triangle_barycentric_coord = q.get_committed_triangle_barycentric_coord();\n"
	"		h.triangle_front_facing = q.is_committed_triangle_front_facing();\n"
	"	}\n"
	"	return h;\n"
	"}\n"
	/* light probes (host_rt_set_probes): the light at points in space - the
	   objects', for the game's lighting of them (object_lights.c) - over
	   64 directions of the sphere: where a ray lands on the level, its
	   lightmap times its colour; where it leaves, the sky's light; the sun
	   where it is not blocked; rays to 8 points of glowing triangles. Out:
	   the light on a surface facing where most of it comes from (in the
	   light buffer's units), how much it comes from there (0 all round, 1
	   all from there), and that direction */
	/* an object's triangle's facing, toward the ray (d): its corners from the
	   frame's object triangles (each instance's opaque ones from .x, its
	   cutouts from .y; .z 0 when it has no opaque ones - its cutouts are then
	   its first geometry) */
	"static float3 object_normal(uint instance, uint geometry, uint primitive, float3 d, device const float *object_tris,\n"
	"	device const uint4 *instance_tris)\n"
	"{\n"
	"	uint4 r = instance_tris[instance];\n"
	"	uint t = (r.z == 0u || geometry == 1u ? r.y : r.x) + primitive;\n"
	"	device const float *v = object_tris + t * 10u;\n"
	"	float3 n = cross(float3(v[3], v[4], v[5]) - float3(v[0], v[1], v[2]), float3(v[6], v[7], v[8]) - float3(v[0], v[1], v[2]));\n"
	"	float l = length(n);\n"
	"	n = l > 1e-8 ? n / l : -d;\n"
	"	return dot(n, d) > 0.0 ? -n : n;\n"
	"}\n"
	/* the light arriving at H, facing N, traced: the sun (and its shadow),
	   one of the sky's wide lights, one glowing triangle (the light grid's
	   choice) - as the path tracer takes it at each place it lands */
	"static float3 hit_light(float3 H, float3 N, thread uint &seed, constant float *c, instance_acceleration_structure world,\n"
	"	device const float4 *glowing, uint glowing_count, GRID_PARAMS, CUT_PARAMS)\n"
	"{\n"
	"	float3 E = float3(0.0);\n"
	"	float3 sun_dir = float3(c[24], c[25], c[26]);\n"
	"	float hs = dot(N, sun_dir);\n"
	"	if (c[27] > 0.0 && hs > 0.0 && c[45] > 0.0)\n"
	"	{\n"
	"		ray to_sun(H, sun_dir, 0.0, 2000.0);\n"
	"		if (!blocked(to_sun, world, 3u, cutout_start, indices, base_texcoords, triangle_materials, materials, object_cutouts,\n"
	"			instance_offsets, mask_rects, masks, cutouts_ready))\n"
	"			E += float3(c[36], c[37], c[38]) * c[45] * hs;\n"
	"	}\n"
	"	uint fills = (c[71] > 0.5 ? 1u : 0u) + (c[79] > 0.5 ? 1u : 0u);\n"
	"	if (fills > 0u)\n"
	"	{\n"
	"		uint chosen = fills == 2u ? (random01(seed) < 0.5 ? 0u : 1u) : (c[71] > 0.5 ? 0u : 1u);\n"
	"		uint o = 64u + chosen * 8u;\n"
	"		float3 axis = float3(c[o], c[o + 1u], c[o + 2u]);\n"
	"		float3 ax_t = normalize(abs(axis.z) < 0.9 ? cross(axis, float3(0, 0, 1)) : cross(axis, float3(1, 0, 0)));\n"
	"		float3 ax_b = cross(axis, ax_t);\n"
	"		float w1 = random01(seed), w2 = random01(seed);\n"
	"		float cos_t = mix(1.0, c[o + 6u], w1), sin_t = sqrt(max(0.0, 1.0 - cos_t * cos_t)), phi = 6.2831853 * w2;\n"
	"		float3 sd = normalize(axis * cos_t + ax_t * (sin_t * cos(phi)) + ax_b * (sin_t * sin(phi)));\n"
	"		float facing = dot(N, sd);\n"
	"		ray to_sky(H, sd, 0.0, 2000.0);\n"
	"		if (facing > 0.0 && !blocked(to_sky, world, 3u, cutout_start, indices, base_texcoords, triangle_materials, materials,\n"
	"			object_cutouts, instance_offsets, mask_rects, masks, cutouts_ready))\n"
	"			E += float3(c[o + 3u], c[o + 4u], c[o + 5u]) * facing * c[45] * float(fills);\n"
	"	}\n"
	"	if (glowing_count > 0u)\n"
	"	{\n"
	"		glow_choice choice = glow_pick(H, random01(seed), glowing, glowing_count, GRID_ARGS);\n"
	"		float4 ga = glowing[choice.index * 3u], gb = glowing[choice.index * 3u + 1u], gc = glowing[choice.index * 3u + 2u];\n"
	"		float r1 = sqrt(random01(seed)), r2 = random01(seed);\n"
	"		float3 at = ga.xyz * (1.0 - r1) + gb.xyz * (r1 * (1.0 - r2)) + gc.xyz * (r1 * r2);\n"
	"		float3 gcross = cross(gb.xyz - ga.xyz, gc.xyz - ga.xyz);\n"
	"		float3 Lg = at - H;\n"
	"		float gd2 = dot(Lg, Lg), gd = sqrt(gd2);\n"
	"		Lg /= max(gd, 1e-4);\n"
	"		float ch = dot(N, Lg), ct = abs(dot(normalize(gcross), Lg));\n"
	"		ray to_glow(H, Lg, 0.0, max(gd - 0.01, 0.0));\n"
	"		if (ch > 0.0 && ct > 0.0 && choice.probability > 0.0 && gd > 1e-3 &&\n"
	"			!blocked(to_glow, world, 3u, cutout_start, indices, base_texcoords, triangle_materials, materials, object_cutouts,\n"
	"				instance_offsets, mask_rects, masks, cutouts_ready))\n"
	"			E += min(materials[uint(gc.w) * 2u + 1u].rgb * c[47] * ch * ct * 0.5 * length(gcross) / (max(gd2, 0.01) * choice.probability) * 0.3183099, float3(c[86]));\n"
	"	}\n"
	"	return E;\n"
	"}\n"
	/* an object's triangle's colour: its texture's average (the tenth float's
	   bits, 8 each of red, green and blue; a mid grey until it is read) */
	"static float3 object_albedo(uint instance, uint geometry, uint primitive, device const float *object_tris,\n"
	"	device const uint4 *instance_tris)\n"
	"{\n"
	"	uint4 r = instance_tris[instance];\n"
	"	uint t = (r.z == 0u || geometry == 1u ? r.y : r.x) + primitive;\n"
	"	uint packed = as_type<uint>(object_tris[t * 10u + 9u]);\n"
	"	if (packed == 0u) return float3(0.45);\n"
	"	return float3(float((packed >> 16) & 255u), float((packed >> 8) & 255u), float(packed & 255u)) / 255.0;\n"
	"}\n"
	"kernel void probes(instance_acceleration_structure world [[buffer(0)]],\n"
	"	constant float *c [[buffer(1)]],\n"
	"	device const uint *indices [[buffer(10)]],\n"
	"	device const float2 *texcoords [[buffer(11)]],\n"
	"	device const uint *triangle_materials [[buffer(12)]],\n"
	"	device const float4 *materials [[buffer(13)]],\n"
	"	device const float4 *pages [[buffer(14)]],\n"
	"	device const float4 *glowing [[buffer(16)]],\n"
	"	constant uint &glowing_count [[buffer(17)]],\n"
	"	constant float4 *grid_info [[buffer(28)]],\n"
	"	device const uint2 *grid_cells [[buffer(29)]],\n"
	"	device const uint2 *grid_entries [[buffer(30)]],\n"
	"	device const float4 *probe_in [[buffer(18)]],\n"
	"	device float4 *probe_out [[buffer(19)]],\n"
	"	constant uint &probe_count [[buffer(20)]],\n"
	"	device const float2 *base_texcoords [[buffer(21)]],\n"
	"	device const float4 *mask_rects [[buffer(22)]],\n"
	"	device const float4 *object_cutouts [[buffer(23)]],\n"
	"	device const uint *instance_offsets [[buffer(24)]],\n"
	"	constant uint &cutouts_ready [[buffer(25)]],\n"
	"	texture2d<float, access::sample> masks [[texture(7)]],\n"
	"	texture2d<float, access::sample> atlas [[texture(3)]],\n"
	"	uint i [[thread_position_in_grid]])\n"
	"{\n"
	"	if (i >= probe_count) return;\n"
	"	float3 P = probe_in[i].xyz;\n"
	"	intersector<triangle_data, instancing> nearest;\n"
	"	nearest.assume_geometry_type(geometry_type::triangle);\n"
	"	nearest.force_opacity(forced_opacity::opaque);\n"
	"	nearest.set_triangle_front_facing_winding(winding::clockwise);\n"
	"	intersector<triangle_data, instancing> blocked_by;\n"
	"	blocked_by.accept_any_intersection(true);\n"
	"	blocked_by.assume_geometry_type(geometry_type::triangle);\n"
	"	blocked_by.force_opacity(forced_opacity::opaque);\n"
	"	const uint N = 64u;\n"
	"	float3 radiance[64];\n"
	"	float3 directions[64];\n"
	"	float3 toward = float3(0.0);\n"
	"	float total = 0.0;\n"
	"	for (uint k = 0; k < N; k++)\n"
	"	{\n"
	/* (a Fibonacci sphere) */
	"		float z = 1.0 - (float(k) + 0.5) * 2.0 / float(N), r = sqrt(max(0.0, 1.0 - z * z));\n"
	"		float phi = float(k) * 2.39996323;\n"
	"		float3 d = float3(r * cos(phi), r * sin(phi), z);\n"
	"		ray probe_ray(P, d, 0.05, 600.0);\n"
	"		hit_info h = closest_hit(probe_ray, world, 1u, CUT_ARGS);\n"
	"		float3 L = float3(0.0);\n"
	"		if (h.type == intersection_type::none)\n"
	"		{\n"
	"			L = float3(c[39], c[40], c[41]);\n"
	"			for (uint s = 0; s < 2u; s++)\n"
	"			{\n"
	"				uint o = 64u + s * 8u;\n"
	"				if (c[o + 7u] > 0.5 && dot(d, float3(c[o], c[o + 1u], c[o + 2u])) > c[o + 6u])\n"
	/* (a wide light's power spread over its cap: 2 pi (1 - cos) of the sphere) */
	"					L += float3(c[o + 3u], c[o + 4u], c[o + 5u]) * c[45] * 3.14159265 / max(6.2831853 * (1.0 - c[o + 6u]), 0.05);\n"
	"			}\n"
	"		}\n"
	"		else if (h.instance_id == 0u && h.triangle_front_facing)\n"
	"		{\n"
	"			uint m = triangle_materials[h.primitive_id];\n"
	"			float4 surface = materials[m * 2u], glow = materials[m * 2u + 1u];\n"
	"			if ((glow.w < 0.0 || pages[uint(glow.w)].z <= 0.0) && c[42] < 1.5)\n"
	"				L = surface.rgb * float3(c[80], c[81], c[82]) * c[43];\n"
	"			else if (c[42] < 1.5)\n"
	"			{\n"
	"				float4 page = pages[uint(glow.w)];\n"
	"				uint base = h.primitive_id * 3u;\n"
	"				float2 b = h.triangle_barycentric_coord;\n"
	"				float2 uv = texcoords[indices[base]] * (1.0 - b.x - b.y) + texcoords[indices[base + 1u]] * b.x +\n"
	"					texcoords[indices[base + 2u]] * b.y;\n"
	"				float2 inset = float2(0.5 / 4096.0);\n"
	"				L = surface.rgb * atlas.sample(linear_clamp, page.xy + clamp(uv * page.zw, inset, page.zw - inset), metal::level(0.0)).rgb * c[43];\n"
	"			}\n"
	"		}\n"
	"		radiance[k] = L;\n"
	"		directions[k] = d;\n"
	"		float luminance = dot(L, float3(0.3, 0.59, 0.11));\n"
	"		toward += d * luminance;\n"
	"		total += luminance;\n"
	"	}\n"
	/* the sun, where it reaches */
	"	float3 sun = float3(c[24], c[25], c[26]);\n"
	"	float3 sun_color = float3(0.0);\n"
	"	if (c[27] > 0.0 && c[45] > 0.0)\n"
	"	{\n"
	"		ray to_sun(P, sun, 0.05, 2000.0);\n"
	"		if (!blocked(to_sun, world, 1u, CUT_ARGS))\n"
	"		{\n"
	"			sun_color = float3(c[36], c[37], c[38]) * c[45];\n"
	"			float luminance = dot(sun_color, float3(0.3, 0.59, 0.11)) * float(N) * 0.25;\n"
	"			toward += sun * luminance;\n"
	"			total += luminance;\n"
	"		}\n"
	"	}\n"
	/* the glowing triangles: 8 rays */
	"	float3 glow_light = float3(0.0), glow_toward = float3(0.0);\n"
	"	uint seed = pcg(i + pcg(uint(c[44])));\n"
	"	for (uint g = 0; g < 8u && glowing_count > 0u; g++)\n"
	"	{\n"
	"		glow_choice choice = glow_pick(P, random01(seed), glowing, glowing_count, GRID_ARGS);\n"
	"		uint lo = choice.index;\n"
	"		float4 a = glowing[lo * 3u], b = glowing[lo * 3u + 1u], cc = glowing[lo * 3u + 2u];\n"
	"		a.w = choice.probability;\n"
	"		float r1 = sqrt(random01(seed)), r2 = random01(seed);\n"
	"		float3 at = a.xyz * (1.0 - r1) + b.xyz * (r1 * (1.0 - r2)) + cc.xyz * (r1 * r2);\n"
	"		float3 cross_ab = cross(b.xyz - a.xyz, cc.xyz - a.xyz);\n"
	"		float3 L = at - P;\n"
	"		float d2 = dot(L, L), d = sqrt(d2);\n"
	"		L /= max(d, 1e-4);\n"
	"		float cos_there = abs(dot(normalize(cross_ab), L));\n"
	"		if (cos_there <= 0.0 || a.w <= 0.0 || d < 1e-3) continue;\n"
	"		ray to_glow(P, L, 0.05, max(d - 0.01, 0.0));\n"
	"		if (blocked(to_glow, world, 1u, CUT_ARGS)) continue;\n"
	"		float3 given = min(materials[uint(cc.w) * 2u + 1u].rgb * c[47] * cos_there * 0.5 * length(cross_ab) / (max(d2, 0.01) * a.w) * 0.3183099, float3(c[86])) / 8.0;\n"
	"		glow_light += given;\n"
	"		glow_toward += L * dot(given, float3(0.3, 0.59, 0.11));\n"
	"	}\n"
	"	toward += glow_toward * float(N) * 0.25;\n"
	"	total += dot(glow_light, float3(0.3, 0.59, 0.11)) * float(N) * 0.25;\n"
	"	float3 D = length(toward) > 1e-5 ? normalize(toward) : float3(0.0, 0.0, 1.0);\n"
	"	float3 E = float3(0.0);\n"
	"	for (uint k = 0; k < N; k++)\n"
	"		E += radiance[k] * max(dot(directions[k], D), 0.0);\n"
	"	E = E * (4.0 / float(N)) + sun_color * max(dot(sun, D), 0.0) + glow_light;\n"
	"	probe_out[i * 2u] = float4(E * c[84] * float3(c[88], c[89], c[90]), total > 0.0 ? clamp(length(toward) / total, 0.0, 1.0) : 0.0);\n"
	"	probe_out[i * 2u + 1u] = float4(D, 1.0);\n"
	"}\n"
	"kernel void trace(texture2d<float, access::read> gbuffer [[texture(0)]],\n"
	"	texture2d<float, access::write> result [[texture(1)]],\n"
	"	texture2d<float, access::write> lit [[texture(2)]],\n"
	"	texture2d<float, access::sample> atlas [[texture(3)]],\n"
	"	texture2d<float, access::read> history_in [[texture(5)]],\n"
	"	texture2d<float, access::write> history_out [[texture(6)]],\n"
	"	instance_acceleration_structure world [[buffer(0)]],\n"
	"	constant float *c [[buffer(1)]],\n"
	"	primitive_acceleration_structure level [[buffer(2)]],\n"
	"	constant float4 *spheres [[buffer(3)]],\n"
	"	constant uint &sphere_count [[buffer(4)]],\n"
	"	device float4 *probe [[buffer(5)]],\n"
	"	constant float4 *lights [[buffer(6)]],\n"
	"	constant uint &light_count [[buffer(7)]],\n"
	"	constant float4 *emitters [[buffer(8)]],\n"
	"	constant uint &emitter_count [[buffer(9)]],\n"
	"	device const uint *indices [[buffer(10)]],\n"
	"	device const float2 *texcoords [[buffer(11)]],\n"
	"	device const uint *triangle_materials [[buffer(12)]],\n"
	"	device const float4 *materials [[buffer(13)]],\n"
	"	device const float4 *pages [[buffer(14)]],\n"
	"	constant uint &gi_ready [[buffer(15)]],\n"
	"	device const float4 *glowing [[buffer(16)]],\n"
	"	constant uint &glowing_count [[buffer(17)]],\n"
	"	constant float4 *grid_info [[buffer(28)]],\n"
	"	device const uint2 *grid_cells [[buffer(29)]],\n"
	"	device const uint2 *grid_entries [[buffer(30)]],\n"
	"	device const float2 *base_texcoords [[buffer(21)]],\n"
	"	device const float4 *mask_rects [[buffer(22)]],\n"
	"	device const float4 *object_cutouts [[buffer(23)]],\n"
	"	device const uint *instance_offsets [[buffer(24)]],\n"
	"	constant uint &cutouts_ready [[buffer(25)]],\n"
	"	texture2d<float, access::sample> masks [[texture(7)]],\n"
	"	device const float *level_vertices [[buffer(26)]],\n"
	"	device const float *object_tris [[buffer(18)]],\n"
	"	device const uint4 *instance_tris [[buffer(19)]],\n"
	"	device atomic_uint *exposure_sums [[buffer(27)]],\n"
	"	uint2 id [[thread_position_in_grid]])\n"
	"{\n"
	"	float2 origin = float2(c[16], c[17]), size = float2(c[18], c[19]);\n"
	"	float2 p = float2(id) + 0.5;\n"
	"	if (any(p < origin) || any(p >= origin + size)) return;\n"
	/* the ray probe (c[33]): the rays of the pixel at the viewport's center */
	"	bool is_probe = c[33] > 0.5 && all(id == uint2(origin + size * 0.5));\n"
	/* (each texture is written once a pixel, on each way out: two writes of
	   one texel from a thread are not ordered) */
	"	uint probe_count = 0u;\n"
	"	if (is_probe) probe[0] = float4(0.0);\n"
	/* the ray view: what a ray from the camera through the pixel finds in
	   Metal's scene - the level's collision triangles each its own colour
	   with its edges drawn, the objects' shapes orange, the player's body
	   cyan - darkened where a ray from the hit to the sun is blocked */
	"	if (c[32] > 0.5 && (c[32] < 1.5 || p.x >= origin.x + size.x * 0.5))\n"
	"	{\n"
	"		float3 eye = float3(c[0], c[1], c[2]), ahead = float3(c[3], c[4], c[5]);\n"
	"		float3 above = float3(c[6], c[7], c[8]), across = float3(c[9], c[10], c[11]);\n"
	"		float2 q = (p - origin) / size * 2.0 - 1.0;\n"
	"		float3 dir = normalize(ahead + across * (q.x * c[14] * c[15]) - above * (q.y * c[14]));\n"
	"		intersector<triangle_data, instancing> view;\n"
	"		view.assume_geometry_type(geometry_type::triangle);\n"
	"		view.force_opacity(forced_opacity::opaque);\n"
	"		view.set_triangle_front_facing_winding(winding::clockwise);\n"
	"		view.set_triangle_cull_mode(triangle_cull_mode::back);\n"
	"		ray primary(eye, dir, c[12], c[13]);\n"
	/* (not the player's body: the camera is inside it) */
	"		hit_info h = closest_hit(primary, world, 3u, CUT_ARGS);\n"
	"		float3 color;\n"
	"		if (h.type == intersection_type::none)\n"
	"			color = mix(float3(0.62, 0.72, 0.9), float3(0.18, 0.28, 0.55), clamp(dir.z * 2.0, 0.0, 1.0));\n"
	"		else\n"
	"		{\n"
	"			float3 base;\n"
	"			if (h.instance_id == 0 && gi_ready != 0u)\n"
	/* (with the drawn level: the light on it, as the rays find it - its
	   lightmap times its colour, and what it gives off; without a page, the
	   lightmaps' average light) */
	"			{\n"
	"				uint m = triangle_materials[h.primitive_id];\n"
	"				float4 surface = materials[m * 2u], glow = materials[m * 2u + 1u];\n"
	"				base = glow.rgb * c[47] + surface.rgb * float3(c[80], c[81], c[82]);\n"
	"				if (glow.w >= 0.0 && pages[uint(glow.w)].z > 0.0)\n"
	"				{\n"
	"					float4 page = pages[uint(glow.w)];\n"
	"					uint base_index = h.primitive_id * 3u;\n"
	"					float2 b = h.triangle_barycentric_coord;\n"
	"					float2 uv = texcoords[indices[base_index]] * (1.0 - b.x - b.y) + texcoords[indices[base_index + 1u]] * b.x +\n"
	"						texcoords[indices[base_index + 2u]] * b.y;\n"
	"					float2 inset = float2(0.5 / 4096.0);\n"
	"					base = glow.rgb * c[47] + surface.rgb * atlas.sample(linear_clamp, page.xy + clamp(uv * page.zw, inset, page.zw - inset), metal::level(0.0)).rgb;\n"
	"				}\n"
	"			}\n"
	"			else if (h.instance_id == 0)\n"
	"			{\n"
	"				uint k = h.primitive_id * 2654435761u;\n"
	"				base = 0.3 + 0.6 * float3(float(k & 255u), float((k >> 8) & 255u), float((k >> 16) & 255u)) / 255.0;\n"
	"				float2 b = h.triangle_barycentric_coord;\n"
	"				if (min(min(b.x, b.y), 1.0 - b.x - b.y) < 0.02) base *= 0.2;\n"
	"			}\n"
	"			else\n"
	"			{\n"
	"				auto body = view.intersect(primary, world, 4u);\n"
	"				bool player = body.type != intersection_type::none && abs(body.distance - h.distance) < 1e-3;\n"
	"				base = player ? float3(0.2, 0.9, 1.0) : float3(1.0, 0.55, 0.15);\n"
	/* (with the traced light: as the rays light it, tinted) */
	"				if (gi_ready != 0u && !player)\n"
	"				{\n"
	"					float3 No = object_normal(h.instance_id, h.geometry_id, h.primitive_id, dir, object_tris, instance_tris);\n"
	"					uint view_seed = pcg(id.x + pcg(id.y + pcg(uint(c[44]))));\n"
	"					base = object_albedo(h.instance_id, h.geometry_id, h.primitive_id, object_tris, instance_tris) * (hit_light(eye + dir * h.distance + No * 0.03, No, view_seed, c, world, glowing,\n"
	"						glowing_count, GRID_ARGS, CUT_ARGS) + float3(c[39], c[40], c[41]));\n"
	"				}\n"
	"			}\n"
	"			if (gi_ready == 0u) base *= 1.0 / (1.0 + h.distance * 0.015);\n"
	"			if (c[27] > 0.0 && gi_ready == 0u)\n"
	"			{\n"
	"				intersector<triangle_data, instancing> sun_hit;\n"
	"				sun_hit.accept_any_intersection(true);\n"
	"				sun_hit.force_opacity(forced_opacity::opaque);\n"
	"				sun_hit.set_triangle_front_facing_winding(winding::clockwise);\n"
	"				sun_hit.set_triangle_cull_mode(triangle_cull_mode::back);\n"
	"				ray to_sun(eye + dir * h.distance - dir * 0.02, float3(c[24], c[25], c[26]), 0.0, 2000.0);\n"
	"				if (sun_hit.intersect(to_sun, world, 7u).type != intersection_type::none) base *= 0.45;\n"
	"			}\n"
	"			color = base;\n"
	"		}\n"
	"		result.write(float4(color, 1.0), id);\n"
	"		lit.write(float4(1.0, 0.0, 0.0, 0.0), id);\n"
	"		return;\n"
	"	}\n"
	"	float4 g = gbuffer.read(id);\n"
	"	float z = abs(g.x);\n"
	/* (nothing there, or at the far plane: no traced light - alpha 0, the
	   game's stays) */
	"	if (z <= c[12] || z >= c[13] * 0.999)\n"
	"	{\n"
	"		result.write(float4(1.0, 0.0, 0.0, 0.0), id);\n"
	"		lit.write(float4(1.0, 0.0, 0.0, 0.0), id);\n"
	"		return;\n"
	"	}\n"
	"	float3 camera = float3(c[0], c[1], c[2]), forward = float3(c[3], c[4], c[5]);\n"
	"	float3 up = float3(c[6], c[7], c[8]), right = float3(c[9], c[10], c[11]);\n"
	"	float t = c[14], aspect = c[15];\n"
	"	float2 ndc = (p - origin) / size * 2.0 - 1.0;\n"
	/* rows run from the top: +y on screen is down */
	"	float3 P = camera + forward * z + right * (ndc.x * t * aspect * z) - up * (ndc.y * t * z);\n"
	"	float3 N = normalize(right * g.y - up * g.z + forward * g.w);\n"
	"	float3 tangent = normalize(abs(N.z) < 0.9 ? cross(N, float3(0, 0, 1)) : cross(N, float3(1, 0, 0)));\n"
	"	float3 bitangent = cross(N, tangent);\n"
	/* the instances' masks: 1 the level, 2 the objects, 4 the player's body
	   (which the game does not draw in the first person) */
	"	bool object = c[23] > 0.5 && g.x < 0.0;\n"
	"	if (is_probe) probe_segment(probe, probe_count, P, P + N * 0.4, 3.0, false);\n"
	"	intersector<triangle_data, instancing> any_hit;\n"
	"	any_hit.accept_any_intersection(true);\n"
	"	any_hit.assume_geometry_type(geometry_type::triangle);\n"
	"	any_hit.force_opacity(forced_opacity::opaque);\n"
	/* rays that can find only the level go through its own structure, not
	   the scene's instances (in compute, a walk through them costs every ray) */
	"	intersector<triangle_data> level_hit;\n"
	"	level_hit.accept_any_intersection(true);\n"
	"	level_hit.assume_geometry_type(geometry_type::triangle);\n"
	"	level_hit.force_opacity(forced_opacity::opaque);\n"
	"	level_hit.set_triangle_front_facing_winding(winding::clockwise);\n"
	"	level_hit.set_triangle_cull_mode(triangle_cull_mode::back);\n"
	/* the level's triangles face outwards, wound counterclockwise around
	   their normal (raytrace_world.c), which Metal's rays see from the front
	   as clockwise (tested: port/macos/tests/run_raytrace_test.sh): a ray
	   leaving a surface from behind it passes */
	"	any_hit.set_triangle_front_facing_winding(winding::clockwise);\n"
	"	any_hit.set_triangle_cull_mode(triangle_cull_mode::back);\n"
	"	float radius = c[21];\n"
	"	float bias = 0.02 + z * 0.002;\n"
	/* near an object (its bounding sphere): the occlusion can find it */
	"	bool near_objects = object;\n"
	"	for (uint s = 0; s < sphere_count && !near_objects; s++)\n"
	"		near_objects = distance(P, spheres[s].xyz) < spheres[s].w + radius;\n"
	/* the level's pixels (marked by the guest): the lightmaps have the
	   level's own occlusion, baked, so their rays find only the objects,
	   which the lightmaps never saw - and none, away from the objects */
	"	bool level_pixel = c[23] > 0.5 && !object;\n"
	/* (with the traced light, a level pixel's occlusion is in it) */
	"	bool traced_level = gi_ready != 0u && c[42] > 0.5 && level_pixel;\n"
	"	float occlusion = 0.0;\n"
	"	for (uint i = 0; i < ((level_pixel && !near_objects) || traced_level ? 0u : 4u); i++)\n"
	"	{\n"
	/* stratified, in a pattern that repeats every 4x4 pixels: the guest's
	   4x4 blur takes in all 16 of its sets of directions (64 in all), so the
	   result is smooth and holds still from frame to frame */
	"		uint k = (id.x & 3u) + 4u * (id.y & 3u);\n"
	"		float u = (float(i) + (float(k) + 0.5) / 16.0) / 4.0;\n"
	"		float v = fract(float(i) * 0.61803399 + float(k) / 16.0);\n"
	/* cosine-weighted over the hemisphere */
	"		float r = sqrt(u), angle = 6.2831853 * v;\n"
	"		float3 d = normalize(tangent * (r * cos(angle)) + bitangent * (r * sin(angle)) + N * sqrt(max(0.0, 1.0 - u)));\n"
	"		ray occlusion_ray(P + N * bias, d, 0.0, radius);\n"
	/* (an object's occlusion: the level's and the other objects'; the
	   level's: the objects' but the player's body, inside which the camera is) */
	"		float distance_hit = -1.0;\n"
	"		if (near_objects)\n"
	"		{\n"
	"			auto hit = any_hit.intersect(occlusion_ray, world, level_pixel ? 2u : 3u);\n"
	"			if (hit.type != intersection_type::none) distance_hit = hit.distance;\n"
	"		}\n"
	"		else\n"
	"		{\n"
	"			auto hit = level_hit.intersect(occlusion_ray, level);\n"
	"			if (hit.type != intersection_type::none) distance_hit = hit.distance;\n"
	"		}\n"
	"		if (distance_hit >= 0.0)\n"
	"			occlusion += 1.0 - distance_hit / radius;\n"
	"		if (is_probe)\n"
	"			probe_segment(probe, probe_count, P + N * bias, P + N * bias + d * (distance_hit >= 0.0 ? distance_hit : radius),\n"
	"				0.0, distance_hit >= 0.0);\n"
	"	}\n"
	"	float visibility = 1.0 - occlusion / 4.0;\n"
	/* the sun's shadow, on what the level's lightmaps do not shade: an
	   object's pixel (the guest marks them; without the marks, a pixel where
	   a short ray into it finds no level surface) facing the sun, whose ray
	   to it the level blocks. The sun is a small disc: the rays spread a
	   little. */
	"	if (c[27] > 0.0)\n"
	"	{\n"
	"		float3 sun = float3(c[24], c[25], c[26]);\n"
	"		bool on_level;\n"
	"		if (c[23] > 0.5) on_level = g.x > 0.0;\n"
	"		else\n"
	"		{\n"
	"			intersector<triangle_data> surface_probe;\n"
	"			surface_probe.accept_any_intersection(true);\n"
	"			surface_probe.force_opacity(forced_opacity::opaque);\n"
	"			ray probe(P + N * bias, -N, 0.0, bias * 3.0);\n"
	"			on_level = surface_probe.intersect(probe, level).type != intersection_type::none;\n"
	"		}\n"
	/* an object's pixel: the level's and the other objects' shadows; the
	   level's: the player's body's only (the lightmaps have the level's,
	   and the game draws the other objects') */
	"		if (dot(N, sun) > 0.0 && !traced_level)\n"
	"		{\n"
	"			uint k = (id.x & 3u) + 4u * (id.y & 3u);\n"
	"			float3 spread = (tangent * (float(k & 3u) - 1.5) + bitangent * (float(k >> 2) - 1.5)) * 0.006;\n"
	/* (an object's pixel's ray starts clear of the object's own surface: its
	   point, from the depth, can be a little inside the drawn model - traced
	   from both sides - which then shadowed all of itself, more or less as
	   the view turned) */
	"			ray shadow_ray(P + N * bias, normalize(sun + spread), on_level ? 0.0 : 0.12 + z * 0.003, 2000.0);\n"
	/* (the level's: only where the body's shadow can fall, near it) */
	"			bool near_body = c[31] > 0.0 && distance(P, float3(c[28], c[29], c[30])) < c[31] * 8.0;\n"
	"			if ((!on_level || near_body) &&\n"
	"				blocked(shadow_ray, world, on_level ? 4u : 3u, CUT_ARGS))\n"
	"				visibility *= 1.0 - 0.55 * c[27];\n"
	"		}\n"
	"	}\n"
	/* the dynamic lights (the flashlight, the plasma's, explosions'): a ray to
	   each that reaches the pixel, blocked by the level or the objects (not
	   the player's body, from which the flashlight shines); the share of
	   their light that arrives, each weighted by how much it gives */
	"	float lights_arriving = 1.0;\n"
	"	float3 emitted = float3(0.0);\n"
	/* (traced lights, c[34]: every light's own, in its colour, in place of
	   the game's dynamic lights on the level; an object's pixel keeps the
	   game's lighting, which has all its lights) */
	"	bool traced_lights = c[34] > 0.5;\n"
	"	bool object_pixel = c[23] > 0.5 && g.x <= 0.0;\n"
	"	if (light_count > 0u && !(traced_lights && object_pixel))\n"
	"	{\n"
	"		float total = 0.0, arriving = 0.0;\n"
	"		for (uint l = 0; l < light_count; l++)\n"
	"		{\n"
	"			float3 at = lights[l * 3u].xyz;\n"
	"			float reach = lights[l * 3u].w;\n"
	"			float3 L = at - P;\n"
	"			float d = length(L);\n"
	"			if (d >= reach || d < 1e-3) continue;\n"
	"			L /= d;\n"
	"			float facing = dot(N, L);\n"
	"			if (facing <= 0.0) continue;\n"
	"			float4 cone = lights[l * 3u + 1u];\n"
	"			if (cone.w > -1.5 && dot(-L, cone.xyz) < cone.w) continue;\n"
	"			float weight = facing * (1.0 - d / reach) * (1.0 - d / reach);\n"
	/* (a spot's edge, soft) */
	"			if (cone.w > -1.5) weight *= smoothstep(cone.w, mix(cone.w, 1.0, 0.25), dot(-L, cone.xyz));\n"
	"			total += weight;\n"
	/* (stopping short of the light by its object's size: the light's own
	   object does not shadow it) */
	"			ray to_light(P + N * bias, L, 0.0, max(d - bias * 4.0 - lights[l * 3u + 2u].x, 0.0));\n"
	"			bool blocked = any_hit.intersect(to_light, world, 3u).type != intersection_type::none;\n"
	"			if (!blocked) arriving += weight;\n"
	"			if (!blocked && traced_lights) emitted += lights[l * 3u + 2u].yzw * weight * 1.5;\n"
	"			if (is_probe) probe_segment(probe, probe_count, P + N * bias, blocked ? P + N * bias + L * d : at, 4.0, blocked);\n"
	"		}\n"
	"		if (traced_lights) lights_arriving = 0.0;\n"
	"		else if (total > 0.0) lights_arriving = arriving / total;\n"
	"	}\n"
	/* the emitters (a needle's glow): their light, where their rays arrive
	   (short of the emitter itself, inside its own model) */
	"	for (uint e = 0; e < emitter_count; e++)\n"
	"	{\n"
	"		float3 at = emitters[e * 2u].xyz;\n"
	"		float reach = emitters[e * 2u].w;\n"
	"		float3 L = at - P;\n"
	"		float d = length(L);\n"
	"		if (d >= reach || d < 1e-3) continue;\n"
	"		L /= d;\n"
	"		float facing = dot(N, L);\n"
	"		if (facing <= 0.0) continue;\n"
	/* (the level only, stopping short: a needle is half in what it stuck in,
	   and inside its own model) */
	"		ray to_emitter(P + N * bias, L, 0.0, max(d - 0.35, 0.0));\n"
	"		bool blocked = level_hit.intersect(to_emitter, level).type != intersection_type::none;\n"
	"		if (!blocked)\n"
	"			emitted += emitters[e * 2u + 1u].rgb * emitters[e * 2u + 1u].w * facing * (1.0 - d / reach) * (1.0 - d / reach);\n"
	"		if (is_probe) probe_segment(probe, probe_count, P + N * bias, at, 5.0, blocked);\n"
	"	}\n"
	"	float4 lit_value = float4(lights_arriving, emitted);\n"
	/* the traced light (c[42]: 1 with the lightmaps' light where the rays
	   land, 2 without - only what the rays find lit: the sun, the sky, the
	   glowing surfaces, the lights), in the light buffer's units, for the
	   guest to put in place of the lightmaps':
	   - the sun, where its ray is not blocked (c[36-38] its colour and
	     power, times c[45]);
	   - two rays across the half sphere, cosine-weighted, new each frame:
	     the sky's light where one leaves the level (c[39-41]); where one
	     lands on the level, the light given off there (c[47]) and, with the
	     lightmaps, the light on it (its lightmap) times its colour (c[43]);
	     accumulated over the frames, followed as the camera moves (the last
	     frame's camera, c[48-59]; c[60] whether there is one);
	   - the traced lights' and the glows' (c[61]). */
	"	if (gi_ready != 0u && c[42] > 0.5)\n"
	"	{\n"
	"		float3 direct = emitted * c[61];\n"
	/* (the drawn level from both sides: its surfaces are the ones drawn, and
	   a ray that leaves through one's back has gone through a wall) */
	"		intersector<triangle_data, instancing> blocked_by;\n"
	"		blocked_by.accept_any_intersection(true);\n"
	"		blocked_by.assume_geometry_type(geometry_type::triangle);\n"
	"		blocked_by.force_opacity(forced_opacity::opaque);\n"
	"		float3 sun_dir = float3(c[24], c[25], c[26]);\n"
	"		float3 sun_color = float3(c[36], c[37], c[38]) * c[45];\n"
	"		float ndl = dot(N, sun_dir);\n"
	"		if (c[27] > 0.0 && ndl > 0.0 && c[45] > 0.0)\n"
	"		{\n"
	"			uint k = (id.x & 3u) + 4u * (id.y & 3u);\n"
	"			float3 spread = (tangent * (float(k & 3u) - 1.5) + bitangent * (float(k >> 2) - 1.5)) * 0.006;\n"
	"			ray to_sun(P + N * bias, normalize(sun_dir + spread), 0.0, 2000.0);\n"
	"			if (!blocked(to_sun, world, object ? 3u : 7u, CUT_ARGS))\n"
	"				direct += sun_color * ndl;\n"
	"		}\n"
	"		intersector<triangle_data, instancing> nearest;\n"
	"		nearest.assume_geometry_type(geometry_type::triangle);\n"
	"		nearest.force_opacity(forced_opacity::opaque);\n"
	"		nearest.set_triangle_front_facing_winding(winding::clockwise);\n"
	"		uint seed = pcg(id.x + pcg(id.y + pcg(uint(c[44]))));\n"
	"		float3 indirect = float3(0.0);\n"
	"		float4 before = float4(0.0);\n"
	"		float weight = 1.0, frames = 1.0;\n"
	"		if (c[60] > 0.5)\n"
	"		{\n"
	"			float3 pp = float3(c[48], c[49], c[50]), pf = float3(c[51], c[52], c[53]);\n"
	"			float3 pu = float3(c[54], c[55], c[56]), pr = float3(c[57], c[58], c[59]);\n"
	"			float3 rel = P - pp;\n"
	"			float pz = dot(rel, pf);\n"
	"			if (pz > c[12])\n"
	"			{\n"
	"				float2 pn = float2(dot(rel, pr) / (pz * t * aspect), -dot(rel, pu) / (pz * t));\n"
	"				float2 ps = origin + (pn * 0.5 + 0.5) * size;\n"
	"				if (all(ps >= origin) && all(ps < origin + size))\n"
	"				{\n"
	/* (its alpha: its depth, in 64ths, and how many samples it holds, below:
	   each new sample takes 1 / that many - their average - down to c[62],
	   the most samples it keeps) */
	"					float4 was = history_in.read(uint2(ps));\n"
	"					float was_frames = fmod(was.a, 256.0), was_z = floor(was.a / 256.0) / 64.0;\n"
	"					if (was_z > 0.0 && abs(was_z - pz) < pz * 0.04 + 0.03)\n"
	"					{\n"
	"						before = was;\n"
	"						frames = min(was_frames + 1.0, 255.0);\n"
	"						weight = max(1.0 / frames, c[62] > 0.0 ? c[62] : 0.03);\n"
	"					}\n"
	"				}\n"
	"			}\n"
	"		}\n"
	/* (new samples every c[63]th frame a pixel, in a scattered pattern,
	   where it has a history; the rest of the time, its history) */
	"		uint period = max(uint(c[63]), 1u);\n"
	/* (and every frame while it holds fewer than 8: a place just come into
	   view fills in at once, not over seconds) */
	"		bool sample_now = weight >= 1.0 || frames <= 8.0 || (pcg(id.x + id.y * 4096u) + uint(c[44])) % period == 0u;\n"
	"		if (!sample_now) frames = fmod(before.a, 256.0);\n"
	"		if (sample_now)\n"
	"		{\n"
	/* (c[93] rays a pixel, their light averaged: the setting's) */
	"		uint spp = uint(clamp(c[93], 1.0, 8.0));\n"
	"		for (uint spp_k = 0u; spp_k < spp; spp_k++)\n"
	"		{\n"
	/* (the sequence's place: the samples, and past the most it counts, on
	   with the frame; each of this frame's rays its own) */
	"		float n = (frames < 254.5 ? frames : frames + float(uint(c[44]) & 4095u)) * float(spp) + float(spp_k);\n"
	/* the sky's wide lights (c[64-79]: each its direction, its colour and
	   power, the cosine of its half width, whether there is one): a ray
	   toward a point of each, new each frame, accumulated with the bounces */
	/* (one of them, chosen at random, for both) */
	"		uint fills = (c[71] > 0.5 ? 1u : 0u) + (c[79] > 0.5 ? 1u : 0u);\n"
	"		uint chosen = fills == 2u ? (random01(seed) < 0.5 ? 0u : 1u) : (c[71] > 0.5 ? 0u : 1u);\n"
	"		for (uint s = chosen; s <= chosen && fills > 0u; s++)\n"
	"		{\n"
	"			uint o = 64u + s * 8u;\n"
	"			float3 axis = float3(c[o], c[o + 1u], c[o + 2u]);\n"
	"			float3 ax_t = normalize(abs(axis.z) < 0.9 ? cross(axis, float3(0, 0, 1)) : cross(axis, float3(1, 0, 0)));\n"
	"			float3 ax_b = cross(axis, ax_t);\n"
	"			float v1 = random01(seed);\n"
	"			float v2 = random01(seed);\n"
	"			float cos_t = mix(1.0, c[o + 6u], v1), sin_t = sqrt(max(0.0, 1.0 - cos_t * cos_t)), phi = 6.2831853 * v2;\n"
	"			float3 d = normalize(axis * cos_t + ax_t * (sin_t * cos(phi)) + ax_b * (sin_t * sin(phi)));\n"
	"			float facing = dot(N, d);\n"
	"			if (facing <= 0.0) continue;\n"
	"			ray to_sky(P + N * bias, d, 0.0, 2000.0);\n"
	"			if (!blocked(to_sky, world, object ? 3u : 7u, CUT_ARGS))\n"
	"				indirect += float3(c[o + 3u], c[o + 4u], c[o + 5u]) * facing * c[45] * float(fills);\n"
	"		}\n"
	/* the bounce: a path over the half sphere, cosine-weighted. Where it
	   lands on the level, the light there - with the lightmaps (c[42] 1),
	   its lightmap times its colour; the path tracer (c[42] 3), the sun,
	   the sky's light and a glowing triangle's traced from there, and on
	   (c[62]... bounces at most 3), its colour taken each time; without
	   (2), what it gives off only (the rays to the glows). Where it leaves
	   the level: the sky's light; where it lands on an object: a dim share
	   of the light around */
	"		{\n"
	"			float2 u = spread01(id, n, 0.0);\n"
	"			float u1 = u.x, u2 = u.y;\n"
	"			float r = sqrt(u1), a = 6.2831853 * u2;\n"
	"			float3 d = normalize(tangent * (r * cos(a)) + bitangent * (r * sin(a)) + N * sqrt(max(0.0, 1.0 - u1)));\n"
	"			float3 origin = P + N * bias;\n"
	"			float3 throughput = float3(1.0);\n"
	"			float3 L = float3(0.0);\n"
	"			uint depth = c[42] > 2.5 ? uint(clamp(c[85], 1.0, 4.0)) : 1u;\n"
	"			for (uint bounce_index = 0; bounce_index < depth; bounce_index++)\n"
	"			{\n"
	"				ray bounce(origin, d, 0.0, 600.0);\n"
	"				hit_info h = closest_hit(bounce, world, 3u, CUT_ARGS);\n"
	"				if (is_probe) probe_segment(probe, probe_count, origin, origin + d * (h.type == intersection_type::none ? 3.0 : h.distance), 5.0, h.type != intersection_type::none);\n"
	"				if (h.type == intersection_type::none)\n"
	"				{\n"
	"					L += throughput * float3(c[39], c[40], c[41]);\n"
	"					break;\n"
	"				}\n"
	/* (an object: lit where it is - the sun, the sky, a glowing triangle -
	   its colour a mid grey; with the path tracer, on from it) */
	"				if (h.instance_id != 0u)\n"
	"				{\n"
	"					float3 No = object_normal(h.instance_id, h.geometry_id, h.primitive_id, d, object_tris, instance_tris);\n"
	"					float3 Ho = origin + d * h.distance + No * bias;\n"
	"					throughput *= object_albedo(h.instance_id, h.geometry_id, h.primitive_id, object_tris, instance_tris);\n"
	"					L += throughput * hit_light(Ho, No, seed, c, world, glowing, glowing_count, GRID_ARGS, CUT_ARGS) *\n"
	"						(c[42] < 1.5 ? c[43] : 1.0);\n"
	"					if (c[42] < 2.5) break;\n"
	"					float o1 = random01(seed), o2 = random01(seed);\n"
	"					float3 ot = normalize(abs(No.z) < 0.9 ? cross(No, float3(0, 0, 1)) : cross(No, float3(1, 0, 0)));\n"
	"					float3 ob = cross(No, ot);\n"
	"					float orr = sqrt(o1), oa = 6.2831853 * o2;\n"
	"					d = normalize(ot * (orr * cos(oa)) + ob * (orr * sin(oa)) + No * sqrt(max(0.0, 1.0 - o1)));\n"
	"					origin = Ho;\n"
	"					continue;\n"
	"				}\n"
	/* (a surface's back: inside a wall, dark) */
	"				if (!h.triangle_front_facing) break;\n"
	"				uint m = triangle_materials[h.primitive_id];\n"
	"				float4 surface = materials[m * 2u], glow = materials[m * 2u + 1u];\n"
	"				uint base = h.primitive_id * 3u;\n"
	"				float2 b = h.triangle_barycentric_coord;\n"
	/* (what it gives off: found by the rays to the glowing triangles) */
	"				if (glowing_count == 0u) L += throughput * glow.rgb * c[47];\n"
	"				if (c[42] < 1.5)\n"
	"				{\n"
	"					if (glow.w < 0.0 || pages[uint(glow.w)].z <= 0.0)\n"
	"						L += throughput * surface.rgb * float3(c[80], c[81], c[82]) * c[43];\n"
	"					else\n"
	"					{\n"
	"						float4 page = pages[uint(glow.w)];\n"
	"						float2 uv = texcoords[indices[base]] * (1.0 - b.x - b.y) + texcoords[indices[base + 1u]] * b.x +\n"
	"							texcoords[indices[base + 2u]] * b.y;\n"
	"						float2 inset = float2(0.5 / 4096.0);\n"
	"						float2 at = page.xy + clamp(uv * page.zw, inset, page.zw - inset);\n"
	"						L += throughput * surface.rgb * atlas.sample(linear_clamp, at, metal::level(0.0)).rgb * c[43];\n"
	"					}\n"
	"					break;\n"
	"				}\n"
	"				if (c[42] < 2.5) break;\n"
	/* the path tracer: the light at the hit, from the sun, a wide sky light
	   and a glowing triangle, traced; then on, from it */
	"				float3 v0 = float3(level_vertices[indices[base] * 3u], level_vertices[indices[base] * 3u + 1u], level_vertices[indices[base] * 3u + 2u]);\n"
	"				float3 v1 = float3(level_vertices[indices[base + 1u] * 3u], level_vertices[indices[base + 1u] * 3u + 1u], level_vertices[indices[base + 1u] * 3u + 2u]);\n"
	"				float3 v2 = float3(level_vertices[indices[base + 2u] * 3u], level_vertices[indices[base + 2u] * 3u + 1u], level_vertices[indices[base + 2u] * 3u + 2u]);\n"
	"				float3 Nh = normalize(cross(v1 - v0, v2 - v0));\n"
	"				if (dot(Nh, d) > 0.0) Nh = -Nh;\n"
	"				float3 H = origin + d * h.distance + Nh * bias;\n"
	"				throughput *= surface.rgb;\n"
	"				float3 E = hit_light(H, Nh, seed, c, world, glowing, glowing_count, GRID_ARGS, CUT_ARGS);\n"
	"				L += throughput * E;\n"
	/* (on: cosine-weighted about the hit's facing) */
	"				float v1r = random01(seed), v2r = random01(seed);\n"
	"				float3 ht = normalize(abs(Nh.z) < 0.9 ? cross(Nh, float3(0, 0, 1)) : cross(Nh, float3(1, 0, 0)));\n"
	"				float3 hb = cross(Nh, ht);\n"
	"				float rr = sqrt(v1r), aa = 6.2831853 * v2r;\n"
	"				d = normalize(ht * (rr * cos(aa)) + hb * (rr * sin(aa)) + Nh * sqrt(max(0.0, 1.0 - v1r)));\n"
	"				origin = H;\n"
	"			}\n"
	"			indirect += L;\n"
	"		}\n"
	/* a ray to a point of a glowing triangle, chosen as likely as the light
	   it gives off: what it gives off where it is not blocked, over the
	   chance of choosing it (the light's share of the half sphere, as the
	   bounces', over pi) */
	"		if (glowing_count > 0u)\n"
	"		{\n"
	"			float2 g = spread01(id, n, 1.0);\n"
	"			glow_choice choice = glow_pick(P, g.x, glowing, glowing_count, GRID_ARGS);\n"
	"			uint lo = choice.index;\n"
	"			float4 a = glowing[lo * 3u], b = glowing[lo * 3u + 1u], cc = glowing[lo * 3u + 2u];\n"
	"			a.w = choice.probability;\n"
	"			float r1 = sqrt(g.y);\n"
	"			float r2 = random01(seed);\n"
	"			float3 at = a.xyz * (1.0 - r1) + b.xyz * (r1 * (1.0 - r2)) + cc.xyz * (r1 * r2);\n"
	"			float3 cross_ab = cross(b.xyz - a.xyz, cc.xyz - a.xyz);\n"
	"			float area = 0.5 * length(cross_ab);\n"
	"			float3 L = at - (P + N * bias);\n"
	"			float d2 = dot(L, L);\n"
	"			float d = sqrt(d2);\n"
	"			L /= max(d, 1e-4);\n"
	"			float cos_here = dot(N, L), cos_there = abs(dot(normalize(cross_ab), L));\n"
	"			if (cos_here > 0.0 && cos_there > 0.0 && a.w > 0.0 && d > 1e-3)\n"
	"			{\n"
	"				ray to_glow(P + N * bias, L, 0.0, max(d - 0.01, 0.0));\n"
	"				if (!blocked(to_glow, world, 3u, CUT_ARGS))\n"
	"				{\n"
	"					float3 given = materials[uint(cc.w) * 2u + 1u].rgb * c[47];\n"
	"					indirect += min(given * cos_here * cos_there * area / (max(d2, 0.01) * a.w) * 0.3183099, float3(c[86]));\n"
	"				}\n"
	"			}\n"
	"		}\n"
	"		}\n"
	"		indirect /= float(spp);\n"
	"		}\n"
	/* (a sample far brighter than the pixel's average - a ray that found a
	   small bright glow, one in thousands - taken down to 6 times it: the
	   denoiser would spread it into a square) */
	"		float sample_brightness = dot(indirect, float3(0.2126, 0.7152, 0.0722));\n"
	"		float ceiling = frames > 4.0 ? max(dot(before.rgb, float3(0.2126, 0.7152, 0.0722)) * 6.0, 0.25) : 4.0;\n"
	"		if (sample_brightness > ceiling) indirect *= ceiling / sample_brightness;\n"
	"		float3 accumulated = sample_now ? mix(before.rgb, indirect, weight) : before.rgb;\n"
	"		history_out.write(float4(accumulated, round(z * 64.0) * 256.0 + frames), id);\n"
	/* (a level pixel: its light goes in the light buffer, occlusion and
	   all - the result's 2 says so, to the guest's light buffer pass and the
	   composite - and the lights' texture carries it: 1, then the light) */
	"		if (!object)\n"
	"		{\n"
	"			visibility = 2.0;\n"
	/* (its first: how many samples it holds, for the denoiser) */
	"			lit_value = float4(frames, (direct + accumulated) * c[84] * float3(c[88], c[89], c[90]));\n"
	"		}\n"
	/* (the exposure's sums: every 8th pixel each way, the level's baked
	   light where a ray from the camera finds the pixel's surface - its
	   lightmap - and the traced light there without the dynamic lights,
	   which the lightmaps have not; in 256ths) */
	"		if (!object && ((id.x | id.y) & 7u) == 0u)\n"
	"		{\n"
	"			float3 to_p = P - camera;\n"
	"			float dist = length(to_p);\n"
	"			ray look(camera, to_p / max(dist, 1e-4), c[12], dist + 0.5);\n"
	"			hit_info lh = closest_hit(look, world, 1u, CUT_ARGS);\n"
	"			if (lh.type != intersection_type::none && lh.instance_id == 0u && abs(lh.distance - dist) < dist * 0.03 + 0.05)\n"
	"			{\n"
	"				uint m = triangle_materials[lh.primitive_id];\n"
	"				float4 glow = materials[m * 2u + 1u];\n"
	"				if (glow.w >= 0.0 && pages[uint(glow.w)].z > 0.0)\n"
	"				{\n"
	"					float4 page = pages[uint(glow.w)];\n"
	"					uint base = lh.primitive_id * 3u;\n"
	"					float2 b = lh.triangle_barycentric_coord;\n"
	"					float2 uv = texcoords[indices[base]] * (1.0 - b.x - b.y) + texcoords[indices[base + 1u]] * b.x +\n"
	"						texcoords[indices[base + 2u]] * b.y;\n"
	"					float2 inset = float2(0.5 / 4096.0);\n"
	"					float3 baked = atlas.sample(linear_clamp, page.xy + clamp(uv * page.zw, inset, page.zw - inset), metal::level(0.0)).rgb;\n"
	"					float3 traced = max(direct - emitted * c[61], float3(0.0)) + accumulated;\n"
	"					float3 luma = float3(0.2126, 0.7152, 0.0722);\n"
	"					atomic_fetch_add_explicit(&exposure_sums[0], uint(min(dot(baked, luma), 16.0) * 256.0), memory_order_relaxed);\n"
	"					atomic_fetch_add_explicit(&exposure_sums[1], uint(min(dot(traced, luma), 16.0) * 256.0), memory_order_relaxed);\n"
	"					atomic_fetch_add_explicit(&exposure_sums[2], 1u, memory_order_relaxed);\n"
	/* (and each colour's, for the white balance) */
	"					for (uint k = 0u; k < 3u; k++)\n"
	"					{\n"
	"						atomic_fetch_add_explicit(&exposure_sums[3u + k], uint(min(baked[k], 16.0) * 256.0), memory_order_relaxed);\n"
	"						atomic_fetch_add_explicit(&exposure_sums[6u + k], uint(min(traced[k], 16.0) * 256.0), memory_order_relaxed);\n"
	"					}\n"
	"				}\n"
	"			}\n"
	"		}\n"
	"	}\n"
	"	lit.write(lit_value, id);\n"
	/* (the probe's ray to the sun: always, against everything, to its first hit) */
	"	if (is_probe && c[27] > 0.0)\n"
	"	{\n"
	"		intersector<triangle_data, instancing> sun_closest;\n"
	"		sun_closest.assume_geometry_type(geometry_type::triangle);\n"
	"		sun_closest.force_opacity(forced_opacity::opaque);\n"
	"		sun_closest.set_triangle_front_facing_winding(winding::clockwise);\n"
	"		sun_closest.set_triangle_cull_mode(triangle_cull_mode::back);\n"
	"		float3 sun = float3(c[24], c[25], c[26]);\n"
	"		ray to_sun(P + N * bias, sun, 0.0, 60.0);\n"
	"		auto sh = sun_closest.intersect(to_sun, world, 7u);\n"
	"		bool blocked = sh.type != intersection_type::none;\n"
	"		probe_segment(probe, probe_count, P + N * bias, P + N * bias + sun * (blocked ? sh.distance : 60.0), 1.0, blocked);\n"
	"	}\n"
	/* the reflection: its hit, where the camera sees it. How much the
	   surface reflects is Fresnel's, more at glancing angles; facing the
	   camera, it reflects so little (4%, times the guest's strength) that
	   the ray is left out, faded in from a twentieth of the full reflection. */
	"	float3 V = normalize(P - camera);\n"
	"	float3 R = reflect(V, N);\n"
	"	float fresnel = mix(0.04, 1.0, pow(1.0 - clamp(dot(-V, N), 0.0, 1.0), 5.0));\n"
	"	float fade = smoothstep(0.05, 0.1, fresnel);\n"
	"	if (fade <= 0.0) { result.write(float4(visibility, 0.0, 0.0, 0.0), id); return; }\n"
	"	intersector<triangle_data> closest;\n"
	"	closest.assume_geometry_type(geometry_type::triangle);\n"
	"	closest.force_opacity(forced_opacity::opaque);\n"
	"	closest.set_triangle_front_facing_winding(winding::clockwise);\n"
	"	closest.set_triangle_cull_mode(triangle_cull_mode::back);\n"
	"	ray reflection_ray(P + N * bias, R, 0.0, c[22]);\n"
	"	auto hit = closest.intersect(reflection_ray, level);\n"
	"	if (is_probe)\n"
	"		probe_segment(probe, probe_count, P + N * bias,\n"
	"			P + N * bias + R * (hit.type != intersection_type::none ? hit.distance : c[22]), 2.0,\n"
	"			hit.type != intersection_type::none);\n"
	"	float4 out = float4(visibility, 0.0, 0.0, 0.0);\n"
	"	if (hit.type != intersection_type::none)\n"
	"	{\n"
	"		float3 H = P + N * bias + R * hit.distance;\n"
	"		float3 relative = H - camera;\n"
	"		float hz = dot(relative, forward);\n"
	"		if (hz > c[12])\n"
	"		{\n"
	"			float2 hit_ndc = float2(dot(relative, right) / (hz * t * aspect), -dot(relative, up) / (hz * t));\n"
	"			float2 screen = origin + (hit_ndc * 0.5 + 0.5) * size;\n"
	"			if (all(screen >= origin) && all(screen < origin + size))\n"
	"			{\n"
	"				float seen = abs(gbuffer.read(uint2(screen)).x);\n"
	/* seen there, not hidden behind something nearer */
	"				if (seen > 0.0 && seen > hz * 0.9 - 0.1)\n"
	"				{\n"
	"					float2 edge = min(screen - origin, origin + size - screen) / (size * 0.08);\n"
	"					out.gb = screen / float2(gbuffer.get_width(), gbuffer.get_height());\n"
	"					out.a = clamp(min(edge.x, edge.y), 0.0, 1.0) * (1.0 - hit.distance / c[22]) * fresnel * fade;\n"
	"				}\n"
	"			}\n"
	"		}\n"
	"	}\n"
	"	result.write(out, id);\n"
	"}\n";

static void *egl_symbol(const char *name)
{
	return (void *)SDL_EGL_GetProcAddress(name);
}

static void *gl_symbol(const char *name)
{
	return (void *)SDL_GL_GetProcAddress(name);
}

int host_rt_available(void)
{
	NSError *error = nil;
	EGLAttrib_ device = 0, metal_device = 0;
	id<MTLLibrary> library;
	id<MTLFunction> function;

	if (rt.checked)
		return rt.available;
	rt.checked = 1;
	rt.eglGetCurrentDisplay = egl_symbol("eglGetCurrentDisplay");
	rt.eglCreateImageKHR = egl_symbol("eglCreateImageKHR");
	rt.eglDestroyImageKHR = egl_symbol("eglDestroyImageKHR");
	rt.eglQueryDisplayAttribEXT = egl_symbol("eglQueryDisplayAttribEXT");
	rt.eglQueryDeviceAttribEXT = egl_symbol("eglQueryDeviceAttribEXT");
	rt.glGenTextures = gl_symbol("glGenTextures");
	rt.glDeleteTextures = gl_symbol("glDeleteTextures");
	rt.glBindTexture = gl_symbol("glBindTexture");
	rt.glTexParameteri = gl_symbol("glTexParameteri");
	rt.glEGLImageTargetTexture2DOES = gl_symbol("glEGLImageTargetTexture2DOES");
	rt.glFinish = gl_symbol("glFinish");
	rt.glFlush = gl_symbol("glFlush");
	rt.eglQueryString = egl_symbol("eglQueryString");
	rt.eglCreateSync = egl_symbol("eglCreateSync");
	rt.eglDestroySync = egl_symbol("eglDestroySync");
	rt.eglWaitSync = egl_symbol("eglWaitSync");
	if (!rt.eglGetCurrentDisplay || !rt.eglCreateImageKHR || !rt.eglQueryDisplayAttribEXT ||
		!rt.eglQueryDeviceAttribEXT || !rt.glEGLImageTargetTexture2DOES || !rt.glFinish || !rt.glGenTextures)
	{
		host_logf(HOST_LOG_INFO, "ray tracing: ANGLE lacks the EGL functions to share Metal textures");
		return 0;
	}
	rt.display = rt.eglGetCurrentDisplay();
	if (!rt.display || !rt.eglQueryDisplayAttribEXT(rt.display, EGL_DEVICE_EXT_, &device) || !device ||
		!rt.eglQueryDeviceAttribEXT((void *)device, EGL_METAL_DEVICE_ANGLE_, &metal_device) || !metal_device)
	{
		host_logf(HOST_LOG_INFO, "ray tracing: ANGLE's Metal device is not available");
		return 0;
	}
	rt.device = (__bridge id<MTLDevice>)(void *)metal_device;
	if (!rt.device.supportsRaytracing)
	{
		host_logf(HOST_LOG_INFO, "ray tracing: %s has no Metal ray tracing", rt.device.name.UTF8String);
		return 0;
	}
	library = [rt.device newLibraryWithSource:kernel_source options:nil error:&error];
	function = library ? [library newFunctionWithName:@"trace"] : nil;
	rt.pipeline = function ? [rt.device newComputePipelineStateWithFunction:function error:&error] : nil;
	{
		id<MTLFunction> probes = library ? [library newFunctionWithName:@"probes"] : nil;

		rt.probe_pipeline = probes ? [rt.device newComputePipelineStateWithFunction:probes error:nil] : nil;
	}
	if (!rt.pipeline)
	{
		host_logf(HOST_LOG_ERROR, "ray tracing: the Metal kernel does not build: %s",
			error ? error.localizedDescription.UTF8String : "?");
		return 0;
	}
	rt.queue = [rt.device newCommandQueue];
	{
		const char *extensions = rt.eglQueryString ? rt.eglQueryString(rt.display, EGL_EXTENSIONS_) : NULL;

		if (extensions && strstr(extensions, "EGL_ANGLE_metal_shared_event_sync") && rt.eglCreateSync &&
			rt.eglDestroySync && rt.eglWaitSync && rt.glFlush && !getenv("HALO_RT_CPU_SYNC"))
		{
			rt.event = [rt.device newSharedEvent];
		}
	}
	rt.available = 1;
	host_logf(HOST_LOG_INFO, "ray tracing: Metal on %s (%s; %s)", rt.device.name.UTF8String,
		[rt.device supportsFamily:MTLGPUFamilyApple9] ? "ray tracing hardware" : "in compute",
		rt.event ? "GL and Metal in turn on the GPU" : "the CPU waits for GL and Metal");
	return 1;
}

/* the level's triangles (vertices: x, y, z; indices: three a triangle) */
int host_rt_set_world(uint32_t generation, const float *vertices, int vertex_count, const uint32_t *indices,
	int triangle_count)
{
	MTLAccelerationStructureTriangleGeometryDescriptor *geometry;
	MTLPrimitiveAccelerationStructureDescriptor *descriptor;
	MTLAccelerationStructureSizes sizes;
	id<MTLBuffer> vertex_buffer, index_buffer, scratch;
	id<MTLCommandBuffer> commands;
	id<MTLAccelerationStructureCommandEncoder> encoder;

	if (!rt.available || generation == rt.world_generation)
		return rt.world != nil;
	rt.world = nil;
	rt.world_generation = generation;
	if (vertex_count <= 0 || triangle_count <= 0)
		return 0;
	vertex_buffer = [rt.device newBufferWithBytes:vertices length:(NSUInteger)vertex_count * 12
		options:MTLResourceStorageModeShared];
	index_buffer = [rt.device newBufferWithBytes:indices length:(NSUInteger)triangle_count * 12
		options:MTLResourceStorageModeShared];
	geometry = [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
	geometry.vertexBuffer = vertex_buffer;
	geometry.vertexStride = 12;
	geometry.indexBuffer = index_buffer;
	geometry.indexType = MTLIndexTypeUInt32;
	geometry.triangleCount = (NSUInteger)triangle_count;
	geometry.opaque = YES;
	descriptor = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
	descriptor.geometryDescriptors = @[ geometry ];
	sizes = [rt.device accelerationStructureSizesWithDescriptor:descriptor];
	rt.world = [rt.device newAccelerationStructureWithSize:sizes.accelerationStructureSize];
	scratch = [rt.device newBufferWithLength:sizes.buildScratchBufferSize options:MTLResourceStorageModePrivate];
	commands = [rt.queue commandBuffer];
	encoder = [commands accelerationStructureCommandEncoder];
	[encoder buildAccelerationStructure:rt.world descriptor:descriptor scratchBuffer:scratch scratchBufferOffset:0];
	[encoder endEncoding];
	[commands commit];
	[commands waitUntilCompleted];
	host_logf(HOST_LOG_INFO, "ray tracing: the level, %d triangles", triangle_count);
	return 1;
}

/* this frame's objects: their triangles in the world (9 floats each) and
each one's group (its object, 0 to 31, above 3 bits of its kind: 2 an
object, 4 the player's body) */
void host_rt_set_objects(const float *triangles, const unsigned char *groups, const float *cutouts, int count,
	int two_sided)
{
	rt.objects_two_sided = two_sided;
	if (!rt.object_triangles)
	{
		rt.object_triangles = malloc(HOST_RT_OBJECT_TRIANGLES * 9 * sizeof(float));
		rt.object_groups = malloc(HOST_RT_OBJECT_TRIANGLES);
		rt.object_cutout_input = malloc(HOST_RT_OBJECT_TRIANGLES * 8 * sizeof(float));
		if (!rt.object_triangles || !rt.object_groups || !rt.object_cutout_input)
			return;
	}
	if (count < 0)
		count = 0;
	if (count > HOST_RT_OBJECT_TRIANGLES)
		count = HOST_RT_OBJECT_TRIANGLES;
	memcpy(rt.object_triangles, triangles, (size_t)count * 9 * sizeof(float));
	memcpy(rt.object_groups, groups, (size_t)count);
	if (cutouts)
		memcpy(rt.object_cutout_input, cutouts, (size_t)count * 8 * sizeof(float));
	else
	{
		int index;

		for (index = 0; index < count; index++)
			rt.object_cutout_input[index * 8 + 6] = -1.0f;
	}
	rt.object_count = count;
}

/* the level the rays see: the drawn one when there is one and it is asked
for, else the collision one */
static id<MTLAccelerationStructure> level_structure(void)
{
	return rt.use_drawn && rt.drawn ? rt.drawn : rt.world;
}

/* the drawn level's triangles (vertices: x, y, z; texcoords: the lightmap's
u, v; indices: three a triangle; each triangle's material) */
int host_rt_set_level(uint32_t generation, const float *vertices, const float *texcoords,
	const float *base_texcoords, int vertex_count, const uint32_t *indices, const uint32_t *triangle_materials,
	int triangle_count, int cutout_start)
{
	MTLAccelerationStructureTriangleGeometryDescriptor *geometry;
	MTLPrimitiveAccelerationStructureDescriptor *descriptor;
	MTLAccelerationStructureSizes sizes;
	id<MTLBuffer> vertex_buffer, scratch;
	id<MTLCommandBuffer> commands;
	id<MTLAccelerationStructureCommandEncoder> encoder;

	if (!rt.available || generation == rt.drawn_generation)
		return rt.drawn != nil;
	rt.drawn = nil;
	rt.drawn_generation = generation;
	/* (a new level: its pages anew) */
	rt.atlas_x = rt.atlas_y = rt.atlas_row = 0;
	if (vertex_count <= 0 || triangle_count <= 0)
		return 0;
	vertex_buffer = [rt.device newBufferWithBytes:vertices length:(NSUInteger)vertex_count * 12
		options:MTLResourceStorageModeShared];
	rt.drawn_vertices = vertex_buffer;
	rt.glowing = nil;
	rt.glowing_count = 0;
	rt.drawn_indices = [rt.device newBufferWithBytes:indices length:(NSUInteger)triangle_count * 12
		options:MTLResourceStorageModeShared];
	rt.drawn_texcoords = [rt.device newBufferWithBytes:texcoords length:(NSUInteger)vertex_count * 8
		options:MTLResourceStorageModeShared];
	rt.drawn_triangle_materials = [rt.device newBufferWithBytes:triangle_materials
		length:(NSUInteger)triangle_count * 4 options:MTLResourceStorageModeShared];
	rt.drawn_base_texcoords = [rt.device newBufferWithBytes:base_texcoords length:(NSUInteger)vertex_count * 8
		options:MTLResourceStorageModeShared];
	if (cutout_start < 0 || cutout_start > triangle_count)
		cutout_start = triangle_count;
	rt.drawn_cutout_start = (uint32_t)cutout_start;
	/* the opaque triangles, then the alpha-tested (geometry 1: the rays test
	their masks) */
	geometry = [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
	geometry.vertexBuffer = vertex_buffer;
	geometry.vertexStride = 12;
	geometry.indexBuffer = rt.drawn_indices;
	geometry.indexType = MTLIndexTypeUInt32;
	geometry.triangleCount = (NSUInteger)(cutout_start > 0 ? cutout_start : triangle_count);
	geometry.opaque = cutout_start > 0 ? YES : NO;
	descriptor = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
	if (cutout_start > 0 && cutout_start < triangle_count)
	{
		MTLAccelerationStructureTriangleGeometryDescriptor *cutouts =
			[MTLAccelerationStructureTriangleGeometryDescriptor descriptor];

		cutouts.vertexBuffer = vertex_buffer;
		cutouts.vertexStride = 12;
		cutouts.indexBuffer = rt.drawn_indices;
		cutouts.indexBufferOffset = (NSUInteger)cutout_start * 12;
		cutouts.indexType = MTLIndexTypeUInt32;
		cutouts.triangleCount = (NSUInteger)(triangle_count - cutout_start);
		cutouts.opaque = NO;
		descriptor.geometryDescriptors = @[ geometry, cutouts ];
	}
	else
	{
		descriptor.geometryDescriptors = @[ geometry ];
	}
	sizes = [rt.device accelerationStructureSizesWithDescriptor:descriptor];
	rt.drawn = [rt.device newAccelerationStructureWithSize:sizes.accelerationStructureSize];
	scratch = [rt.device newBufferWithLength:sizes.buildScratchBufferSize options:MTLResourceStorageModePrivate];
	commands = [rt.queue commandBuffer];
	encoder = [commands accelerationStructureCommandEncoder];
	[encoder buildAccelerationStructure:rt.drawn descriptor:descriptor scratchBuffer:scratch scratchBufferOffset:0];
	[encoder endEncoding];
	[commands commit];
	[commands waitUntilCompleted];
	host_logf(HOST_LOG_INFO, "ray tracing: the drawn level, %d triangles, %d vertices", triangle_count, vertex_count);
	return 1;
}

/* the light grid (rt.grid_*): the level's box (lo to hi) cut into at most
16384 cells, each listing the 32 glowing triangles (12 floats each: a, its
chance; b, the running chance; c) whose light over their distance squared
to its middle is greatest, by index, each with the running share of that
(a float's bits) - built on a queue of its own, the latest asked for */
#define LIGHT_GRID_CELL_TRIANGLES 32

static void light_grid_build(const float *glow, uint32_t count, const float *lo, const float *hi,
	__unsafe_unretained id<MTLBuffer> source)
{
	float extent[3], size, *center_x, *power;
	int dims[3], axis;
	uint32_t cells, cell, *cell_data, *entries, used = 0, index;
	id<MTLBuffer> info_buffer, cells_buffer, entries_buffer;

	for (axis = 0; axis < 3; axis++)
		extent[axis] = hi[axis] - lo[axis] + 1.0f;
	size = cbrtf(extent[0] * extent[1] * extent[2] / 16384.0f);
	if (size < 3.0f)
		size = 3.0f;
	for (axis = 0; axis < 3; axis++)
	{
		dims[axis] = (int)ceilf(extent[axis] / size);
		dims[axis] = dims[axis] < 1 ? 1 : dims[axis] > 128 ? 128 : dims[axis];
	}
	cells = (uint32_t)(dims[0] * dims[1] * dims[2]);
	center_x = malloc(count * 3 * sizeof(float));
	power = malloc(count * sizeof(float));
	cell_data = malloc(cells * 2 * sizeof(uint32_t));
	entries = malloc((size_t)cells * LIGHT_GRID_CELL_TRIANGLES * 2 * sizeof(uint32_t));
	if (!center_x || !power || !cell_data || !entries)
	{
		free(center_x);
		free(power);
		free(cell_data);
		free(entries);
		return;
	}
	for (index = 0; index < count; index++)
	{
		for (axis = 0; axis < 3; axis++)
			center_x[index * 3 + axis] = (glow[index * 12 + axis] + glow[index * 12 + 4 + axis] +
				glow[index * 12 + 8 + axis]) / 3.0f;
		power[index] = glow[index * 12 + 3];
	}
	for (cell = 0; cell < cells; cell++)
	{
		uint32_t best_index[LIGHT_GRID_CELL_TRIANGLES];
		float best_weight[LIGHT_GRID_CELL_TRIANGLES], middle[3], near = size * size * 0.25f, total = 0.0f, running = 0.0f;
		int kept = 0, weakest = 0, k, j;

		middle[0] = lo[0] + ((float)(cell % (uint32_t)dims[0]) + 0.5f) * size;
		middle[1] = lo[1] + ((float)((cell / (uint32_t)dims[0]) % (uint32_t)dims[1]) + 0.5f) * size;
		middle[2] = lo[2] + ((float)(cell / (uint32_t)(dims[0] * dims[1])) + 0.5f) * size;
		for (index = 0; index < count; index++)
		{
			float dx = center_x[index * 3] - middle[0], dy = center_x[index * 3 + 1] - middle[1];
			float dz = center_x[index * 3 + 2] - middle[2], d2 = dx * dx + dy * dy + dz * dz;
			float weight = power[index] / (d2 > near ? d2 : near);

			if (kept < LIGHT_GRID_CELL_TRIANGLES)
			{
				best_index[kept] = index;
				best_weight[kept] = weight;
				kept++;
			}
			else if (weight > best_weight[weakest])
			{
				best_index[weakest] = index;
				best_weight[weakest] = weight;
			}
			else
				continue;
			/* (the weakest kept, again) */
			if (kept == LIGHT_GRID_CELL_TRIANGLES)
			{
				weakest = 0;
				for (k = 1; k < kept; k++)
					if (best_weight[k] < best_weight[weakest])
						weakest = k;
			}
		}
		/* (by index, for the kernel's search; the running share) */
		for (k = 1; k < kept; k++)
			for (j = k; j > 0 && best_index[j - 1] > best_index[j]; j--)
			{
				uint32_t swap_index = best_index[j];
				float swap_weight = best_weight[j];

				best_index[j] = best_index[j - 1];
				best_weight[j] = best_weight[j - 1];
				best_index[j - 1] = swap_index;
				best_weight[j - 1] = swap_weight;
			}
		for (k = 0; k < kept; k++)
			total += best_weight[k];
		cell_data[cell * 2] = used;
		cell_data[cell * 2 + 1] = total > 0.0f ? (uint32_t)kept : 0;
		for (k = 0; k < kept && total > 0.0f; k++)
		{
			float share;

			running += best_weight[k] / total;
			share = k == kept - 1 ? 1.0f : running;
			entries[(used + (uint32_t)k) * 2] = best_index[k];
			memcpy(&entries[(used + (uint32_t)k) * 2 + 1], &share, sizeof(share));
		}
		if (total > 0.0f)
			used += (uint32_t)kept;
	}
	{
		float info[8] = { lo[0], lo[1], lo[2], size, (float)dims[0], (float)dims[1], (float)dims[2], 1.0f };

		info_buffer = [rt.device newBufferWithBytes:info length:sizeof(info) options:MTLResourceStorageModeShared];
		cells_buffer = [rt.device newBufferWithBytes:cell_data length:cells * 2 * sizeof(uint32_t)
			options:MTLResourceStorageModeShared];
		entries_buffer = [rt.device newBufferWithBytes:entries length:(used ? used : 1) * 2 * sizeof(uint32_t)
			options:MTLResourceStorageModeShared];
	}
	free(center_x);
	free(power);
	free(cell_data);
	free(entries);
	@synchronized (rt.queue)
	{
		rt.grid_info = info_buffer;
		rt.grid_cells = cells_buffer;
		rt.grid_entries = entries_buffer;
		rt.grid_glowing = source;
	}
	host_logf(HOST_LOG_INFO, "ray tracing: the light grid, %dx%dx%d cells of %.1f, %u glowing triangles", dims[0], dims[1],
		dims[2], size, count);
}

/* the light grid asked for: the glowing triangles now (copied), built on its
queue when it is free - one build at a time, of the latest asked for */
static void light_grid_ask(void)
{
	static dispatch_queue_t queue;
	static float *pending;
	static uint32_t pending_count;
	static float pending_lo[3], pending_hi[3];
	static __unsafe_unretained id<MTLBuffer> pending_source;
	static int scheduled;
	const float *vertices = rt.drawn_vertices.contents;
	NSUInteger vertex_count = rt.drawn_vertices.length / 12, index;
	float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f }, *copy;
	int axis;

	if (!rt.glowing || !rt.glowing_count || !vertices || !vertex_count)
		return;
	/* (the box: the glowing triangles', 40 units more all round, within the
	level's - b30's sea reaches 1000 out, and cells over it were 42 units) */
	{
		const float *glow = rt.glowing.contents;
		float level_lo[3] = { 1e30f, 1e30f, 1e30f }, level_hi[3] = { -1e30f, -1e30f, -1e30f };

		for (index = 0; index < vertex_count; index++)
			for (axis = 0; axis < 3; axis++)
			{
				float value = vertices[index * 3 + axis];

				level_lo[axis] = value < level_lo[axis] ? value : level_lo[axis];
				level_hi[axis] = value > level_hi[axis] ? value : level_hi[axis];
			}
		for (index = 0; index < rt.glowing_count; index++)
		{
			int corner;

			for (corner = 0; corner < 3; corner++)
				for (axis = 0; axis < 3; axis++)
				{
					float value = glow[index * 12 + corner * 4 + axis];

					lo[axis] = value < lo[axis] ? value : lo[axis];
					hi[axis] = value > hi[axis] ? value : hi[axis];
				}
		}
		for (axis = 0; axis < 3; axis++)
		{
			lo[axis] = fmaxf(lo[axis] - 40.0f, level_lo[axis]);
			hi[axis] = fminf(hi[axis] + 40.0f, level_hi[axis]);
		}
	}
	copy = malloc(rt.glowing_count * 12 * sizeof(float));
	if (!copy)
		return;
	memcpy(copy, rt.glowing.contents, rt.glowing_count * 12 * sizeof(float));
	if (!queue)
		queue = dispatch_queue_create("halo.light_grid", DISPATCH_QUEUE_SERIAL);
	@synchronized (rt.queue)
	{
		free(pending);
		pending = copy;
		pending_count = rt.glowing_count;
		memcpy(pending_lo, lo, sizeof(lo));
		memcpy(pending_hi, hi, sizeof(hi));
		pending_source = rt.glowing;
		if (scheduled)
			return;
		scheduled = 1;
	}
	dispatch_async(queue, ^{
		for (;;)
		{
			float *glow, build_lo[3], build_hi[3];
			uint32_t build_count;
			__unsafe_unretained id<MTLBuffer> source;

			@synchronized (rt.queue)
			{
				if (!pending)
				{
					scheduled = 0;
					return;
				}
				glow = pending;
				pending = NULL;
				build_count = pending_count;
				memcpy(build_lo, pending_lo, sizeof(build_lo));
				memcpy(build_hi, pending_hi, sizeof(build_hi));
				source = pending_source;
			}
			light_grid_build(glow, build_count, build_lo, build_hi, source);
			free(glow);
		}
	});
}

/* the drawn level's materials: 8 floats each (the colour, the flags; the
light given off, the lightmap page or -1) */
void host_rt_set_level_materials(const float *materials, int count)
{
	if (!rt.available || count <= 0)
		return;
	rt.drawn_materials = [rt.device newBufferWithBytes:materials length:(NSUInteger)count * 32
		options:MTLResourceStorageModeShared];
	/* the glowing triangles, each as likely as the light it gives off (its
	glow's brightness times its area) */
	if (rt.drawn && rt.drawn_vertices && rt.drawn_indices && rt.drawn_triangle_materials)
	{
		const float *vertices = rt.drawn_vertices.contents;
		const uint32_t *indices = rt.drawn_indices.contents, *triangle_materials = rt.drawn_triangle_materials.contents;
		NSUInteger triangle_count = rt.drawn_indices.length / 12, index, glowing = 0;
		double total = 0.0, sum = 0.0;
		float *out;

		for (index = 0; index < triangle_count; index++)
		{
			uint32_t m = triangle_materials[index];

			if ((int)m < count && materials[m * 8 + 4] + materials[m * 8 + 5] + materials[m * 8 + 6] > 0.0f)
				glowing++;
		}
		rt.glowing = nil;
		rt.glowing_count = 0;
		if (!glowing)
			return;
		rt.glowing = [rt.device newBufferWithLength:glowing * 48 options:MTLResourceStorageModeShared];
		out = rt.glowing.contents;
		for (index = 0; index < triangle_count; index++)
		{
			uint32_t m = triangle_materials[index];
			const float *a, *b, *c;
			float u[3], v[3], n[3], power;

			if ((int)m >= count || !(materials[m * 8 + 4] + materials[m * 8 + 5] + materials[m * 8 + 6] > 0.0f))
				continue;
			a = vertices + indices[index * 3] * 3;
			b = vertices + indices[index * 3 + 1] * 3;
			c = vertices + indices[index * 3 + 2] * 3;
			u[0] = b[0] - a[0]; u[1] = b[1] - a[1]; u[2] = b[2] - a[2];
			v[0] = c[0] - a[0]; v[1] = c[1] - a[1]; v[2] = c[2] - a[2];
			n[0] = u[1] * v[2] - u[2] * v[1];
			n[1] = u[2] * v[0] - u[0] * v[2];
			n[2] = u[0] * v[1] - u[1] * v[0];
			power = 0.5f * sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) *
				(materials[m * 8 + 4] + materials[m * 8 + 5] + materials[m * 8 + 6]);
			out[rt.glowing_count * 12 + 0] = a[0];
			out[rt.glowing_count * 12 + 1] = a[1];
			out[rt.glowing_count * 12 + 2] = a[2];
			out[rt.glowing_count * 12 + 3] = power;
			out[rt.glowing_count * 12 + 4] = b[0];
			out[rt.glowing_count * 12 + 5] = b[1];
			out[rt.glowing_count * 12 + 6] = b[2];
			out[rt.glowing_count * 12 + 8] = c[0];
			out[rt.glowing_count * 12 + 9] = c[1];
			out[rt.glowing_count * 12 + 10] = c[2];
			out[rt.glowing_count * 12 + 11] = (float)m;
			total += power;
			rt.glowing_count++;
		}
		for (index = 0; index < rt.glowing_count; index++)
		{
			float chance = total > 0.0 ? (float)(out[index * 12 + 3] / total) : 0.0f;

			sum += chance;
			out[index * 12 + 3] = chance;
			out[index * 12 + 7] = (float)sum;
		}
		{
			static uint32_t logged = (uint32_t)-1;

			if (logged != rt.glowing_count)
				host_logf(HOST_LOG_INFO, "ray tracing: %u glowing triangles", rt.glowing_count);
			logged = rt.glowing_count;
		}
		light_grid_ask();
	}
}

/* the points to light probe this frame (x, y, z each; at most 64) */
void host_rt_set_probes(const float *points, int count)
{
	int index;

	if (count > 64)
		count = 64;
	for (index = 0; index < count && index < 64; index++)
	{
		rt.probe_points[index * 4 + 0] = points[index * 3 + 0];
		rt.probe_points[index * 4 + 1] = points[index * 3 + 1];
		rt.probe_points[index * 4 + 2] = points[index * 3 + 2];
		rt.probe_points[index * 4 + 3] = 0.0f;
	}
	rt.probe_count = count > 0 ? (uint32_t)count : 0;
}

/* the last probes done: 10 floats each (the point; the light; how much of
it from one way; that way); how many */
int host_rt_probe_results(float *results, int maximum)
{
	int count;

	if (!rt.available)
		return 0;
	@synchronized (rt.queue)
	{
		count = rt.probe_result_count < maximum ? rt.probe_result_count : maximum;
		memcpy(results, rt.probe_results, (size_t)count * 10 * sizeof(float));
	}
	return count;
}

#define HOST_RT_MASK_ATLAS_SIZE 2048
#define HOST_RT_MASKS 256

/* a cutout's mask (a byte of alpha a texel), into the masks' texture (rows
of masks); generation: the masks start anew when it changes */
int host_rt_set_mask(uint32_t generation, int index, int width, int height, const unsigned char *alpha)
{
	float *rect;

	if (!rt.available || index < 0 || index >= HOST_RT_MASKS || width <= 0 || height <= 0 || width > 256 ||
		height > 256)
	{
		return 0;
	}
	if (!rt.mask_atlas)
	{
		MTLTextureDescriptor *descriptor = [MTLTextureDescriptor
			texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm width:HOST_RT_MASK_ATLAS_SIZE
			height:HOST_RT_MASK_ATLAS_SIZE mipmapped:NO];

		descriptor.usage = MTLTextureUsageShaderRead;
		descriptor.storageMode = MTLStorageModeShared;
		rt.mask_atlas = [rt.device newTextureWithDescriptor:descriptor];
		rt.mask_rects = [rt.device newBufferWithLength:HOST_RT_MASKS * 16 options:MTLResourceStorageModeShared];
		if (!rt.mask_atlas || !rt.mask_rects)
			return 0;
		memset(rt.mask_rects.contents, 0, HOST_RT_MASKS * 16);
	}
	if (generation != rt.mask_generation)
	{
		rt.mask_generation = generation;
		rt.mask_x = rt.mask_y = rt.mask_row = 0;
		memset(rt.mask_rects.contents, 0, HOST_RT_MASKS * 16);
	}
	if (rt.mask_x + width > HOST_RT_MASK_ATLAS_SIZE)
	{
		rt.mask_x = 0;
		rt.mask_y += rt.mask_row;
		rt.mask_row = 0;
	}
	if (rt.mask_y + height > HOST_RT_MASK_ATLAS_SIZE)
		return 0;
	[rt.mask_atlas replaceRegion:MTLRegionMake2D((NSUInteger)rt.mask_x, (NSUInteger)rt.mask_y, (NSUInteger)width,
		(NSUInteger)height) mipmapLevel:0 withBytes:alpha bytesPerRow:(NSUInteger)width];
	rect = (float *)rt.mask_rects.contents + index * 4;
	rect[0] = (float)rt.mask_x / HOST_RT_MASK_ATLAS_SIZE;
	rect[1] = (float)rt.mask_y / HOST_RT_MASK_ATLAS_SIZE;
	rect[2] = (float)width / HOST_RT_MASK_ATLAS_SIZE;
	rect[3] = (float)height / HOST_RT_MASK_ATLAS_SIZE;
	rt.mask_x += width;
	if (height > rt.mask_row)
		rt.mask_row = height;
	return 1;
}

#define HOST_RT_ATLAS_SIZE 4096
#define HOST_RT_PAGES 256

/* a lightmap page (RGBA bytes), into the pages' texture: rows of pages,
each row as tall as its tallest; 0 if it does not fit */
int host_rt_set_level_page(int page, int width, int height, const unsigned char *pixels)
{
	float *table;

	if (!rt.available || page < 0 || page >= HOST_RT_PAGES || width <= 0 || height <= 0 ||
		width > HOST_RT_ATLAS_SIZE || height > HOST_RT_ATLAS_SIZE)
	{
		return 0;
	}
	if (!rt.atlas)
	{
		MTLTextureDescriptor *descriptor = [MTLTextureDescriptor
			texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:HOST_RT_ATLAS_SIZE
			height:HOST_RT_ATLAS_SIZE mipmapped:NO];

		descriptor.usage = MTLTextureUsageShaderRead;
		descriptor.storageMode = MTLStorageModeShared;
		rt.atlas = [rt.device newTextureWithDescriptor:descriptor];
		rt.drawn_pages = [rt.device newBufferWithLength:HOST_RT_PAGES * 16 options:MTLResourceStorageModeShared];
		memset(rt.drawn_pages.contents, 0, HOST_RT_PAGES * 16);
		if (!rt.atlas || !rt.drawn_pages)
			return 0;
	}
	if (rt.atlas_x + width > HOST_RT_ATLAS_SIZE)
	{
		rt.atlas_x = 0;
		rt.atlas_y += rt.atlas_row;
		rt.atlas_row = 0;
	}
	if (rt.atlas_y + height > HOST_RT_ATLAS_SIZE)
	{
		host_logf(HOST_LOG_ERROR, "ray tracing: lightmap page %d (%dx%d) does not fit", page, width, height);
		return 0;
	}
	[rt.atlas replaceRegion:MTLRegionMake2D((NSUInteger)rt.atlas_x, (NSUInteger)rt.atlas_y, (NSUInteger)width,
		(NSUInteger)height) mipmapLevel:0 withBytes:pixels bytesPerRow:(NSUInteger)width * 4];
	table = (float *)rt.drawn_pages.contents + page * 4;
	table[0] = (float)rt.atlas_x / HOST_RT_ATLAS_SIZE;
	table[1] = (float)rt.atlas_y / HOST_RT_ATLAS_SIZE;
	table[2] = (float)width / HOST_RT_ATLAS_SIZE;
	table[3] = (float)height / HOST_RT_ATLAS_SIZE;
	rt.atlas_x += width;
	if (height > rt.atlas_row)
		rt.atlas_row = height;
	return 1;
}

/* how far a group's triangles moved since the last frame's (the ring's
last), the most any corner did (0: none) */
static float body_moved(int ring, int group, int triangles, int cutouts)
{
	int last = (ring + 2) % 3, kind;
	float most = 0.0f;

	for (kind = 0; kind < 2; kind++)
	{
		const float *now = (const float *)(kind ? rt.cut_vertices : rt.body_vertices)[ring][group].contents;
		const float *then = (const float *)(kind ? rt.cut_vertices : rt.body_vertices)[last][group].contents;
		int count = (kind ? cutouts : triangles) * 9, index;

		for (index = 0; index < count; index++)
		{
			float moved = fabsf(now[index] - then[index]);

			if (moved > most)
				most = moved;
		}
	}
	return most;
}

/* the scene for this frame's rays: each object's mesh, and the level and
them, built in the command buffer before the rays */
static int encode_scene(id<MTLCommandBuffer> commands)
{
	MTLInstanceAccelerationStructureDescriptor *descriptor;
	MTLAccelerationStructureInstanceDescriptor *instances;
	NSMutableArray<id<MTLAccelerationStructure>> *structures;
	id<MTLBuffer> buffer;
	id<MTLAccelerationStructureCommandEncoder> encoder;
	int index, group, ring = rt.instance_ring, count;
	int group_triangles[HOST_RT_GROUPS] = { 0 }, group_cutouts[HOST_RT_GROUPS] = { 0 };
	int cutout_offsets[HOST_RT_GROUPS] = { 0 }, cutout_fill[HOST_RT_GROUPS] = { 0 }, cutout_total = 0;
	unsigned char group_masks[HOST_RT_GROUPS] = { 0 };
	uint32_t *offsets;
	float *cutout_attributes;

	if (!rt.instance_buffers[0])
	{
		for (index = 0; index < 3; index++)
		{
			rt.instance_buffers[index] = [rt.device newBufferWithLength:(HOST_RT_GROUPS + 1) *
				sizeof(MTLAccelerationStructureInstanceDescriptor) options:MTLResourceStorageModeShared];
			for (group = 0; group < HOST_RT_GROUPS; group++)
			{
				rt.body_vertices[index][group] = [rt.device newBufferWithLength:HOST_RT_GROUP_TRIANGLES * 9 *
					sizeof(float) options:MTLResourceStorageModeShared];
				rt.cut_vertices[index][group] = [rt.device newBufferWithLength:HOST_RT_GROUP_TRIANGLES * 9 *
					sizeof(float) options:MTLResourceStorageModeShared];
			}
			rt.object_cutouts[index] = [rt.device newBufferWithLength:HOST_RT_OBJECT_TRIANGLES * 32
				options:MTLResourceStorageModeShared];
			rt.instance_offsets[index] = [rt.device newBufferWithLength:(HOST_RT_GROUPS + 1) * 4
				options:MTLResourceStorageModeShared];
			rt.object_tris[index] = [rt.device newBufferWithLength:(NSUInteger)HOST_RT_OBJECT_TRIANGLES * 40
				options:MTLResourceStorageModeShared];
			rt.instance_tris[index] = [rt.device newBufferWithLength:(HOST_RT_GROUPS + 1) * 16
				options:MTLResourceStorageModeShared];
		}
	}
	rt.instance_ring = (ring + 1) % 3;
	rt.scene_ring = ring;
	/* each object's triangles, into its mesh: the opaque, and the cutouts
	(alpha-tested: the rays test their masks), whose coordinates and masks go
	in the frame's cutouts, each instance's from its first */
	for (index = 0; index < rt.object_count; index++)
	{
		group = rt.object_groups[index] >> 3;
		if (group < HOST_RT_GROUPS && rt.object_cutout_input[index * 8 + 6] >= 0.0f)
			group_cutouts[group]++;
	}
	for (group = 0; group < HOST_RT_GROUPS; group++)
	{
		if (group_cutouts[group] > HOST_RT_GROUP_TRIANGLES)
			group_cutouts[group] = HOST_RT_GROUP_TRIANGLES;
		cutout_offsets[group] = cutout_total;
		cutout_total += group_cutouts[group];
	}
	cutout_attributes = (float *)rt.object_cutouts[ring].contents;
	for (index = 0; index < rt.object_count; index++)
	{
		unsigned char mask = rt.object_groups[index];
		const float *cut = &rt.object_cutout_input[index * 8];
		float *vertices;

		group = mask >> 3;
		if (group >= HOST_RT_GROUPS)
			continue;
		group_masks[group] = mask & 7;
		if (cut[6] >= 0.0f)
		{
			float *attributes;

			if (cutout_fill[group] >= group_cutouts[group])
				continue;
			vertices = (float *)rt.cut_vertices[ring][group].contents + cutout_fill[group] * 9;
			attributes = cutout_attributes + (cutout_offsets[group] + cutout_fill[group]) * 8;
			memcpy(attributes, cut, 8 * sizeof(float));
			cutout_fill[group]++;
		}
		else
		{
			if (group_triangles[group] >= HOST_RT_GROUP_TRIANGLES)
				continue;
			vertices = (float *)rt.body_vertices[ring][group].contents + group_triangles[group] * 9;
			group_triangles[group]++;
		}
		memcpy(vertices, &rt.object_triangles[index * 9], 9 * sizeof(float));
	}
	/* (and all of them, for the kernel: each group's opaque ones, then its
	cutouts, each with its colour - as they were put in the groups above) */
	{
		int opaque_base[HOST_RT_GROUPS], cutout_base[HOST_RT_GROUPS], opaque_at[HOST_RT_GROUPS] = { 0 },
			cutout_at[HOST_RT_GROUPS] = { 0 }, at = 0;
		float *all = (float *)rt.object_tris[ring].contents;

		for (group = 0; group < HOST_RT_GROUPS; group++)
		{
			opaque_base[group] = at;
			at += group_triangles[group];
			cutout_base[group] = at;
			at += cutout_fill[group];
			rt.tri_base[group][0] = (uint32_t)opaque_base[group];
			rt.tri_base[group][1] = (uint32_t)cutout_base[group];
		}
		for (index = 0; index < rt.object_count; index++)
		{
			const float *cut = &rt.object_cutout_input[index * 8];
			float *out;

			group = rt.object_groups[index] >> 3;
			if (group >= HOST_RT_GROUPS)
				continue;
			if (cut[6] >= 0.0f)
			{
				if (cutout_at[group] >= cutout_fill[group])
					continue;
				out = all + (cutout_base[group] + cutout_at[group]++) * 10;
			}
			else
			{
				if (opaque_at[group] >= group_triangles[group])
					continue;
				out = all + (opaque_base[group] + opaque_at[group]++) * 10;
			}
			memcpy(out, &rt.object_triangles[index * 9], 9 * sizeof(float));
			memcpy(out + 9, &cut[7], sizeof(float));
		}
	}
	offsets = (uint32_t *)rt.instance_offsets[ring].contents;
	memset(offsets, 0, (HOST_RT_GROUPS + 1) * 4);
	memset(rt.instance_tris[ring].contents, 0, (HOST_RT_GROUPS + 1) * 16);
	encoder = [commands accelerationStructureCommandEncoder];
	structures = [NSMutableArray arrayWithObject:level_structure()];
	buffer = rt.instance_buffers[ring];
	instances = (MTLAccelerationStructureInstanceDescriptor *)buffer.contents;
	memset(instances, 0, (HOST_RT_GROUPS + 1) * sizeof(*instances));
	for (index = 0; index <= HOST_RT_GROUPS; index++)
	{
		/* (not forced opaque: the cutouts' geometries are not) */
		instances[index].options = MTLAccelerationStructureInstanceOptionNone;
		instances[index].transformationMatrix.columns[0] = MTLPackedFloat3Make(1, 0, 0);
		instances[index].transformationMatrix.columns[1] = MTLPackedFloat3Make(0, 1, 0);
		instances[index].transformationMatrix.columns[2] = MTLPackedFloat3Make(0, 0, 1);
	}
	instances[0].mask = 1;
	count = 1;
	for (group = 0; group < HOST_RT_GROUPS; group++)
	{
		MTLAccelerationStructureTriangleGeometryDescriptor *geometry, *cutouts;
		MTLPrimitiveAccelerationStructureDescriptor *mesh;

		/* (none this frame: its mesh, built from what is gone, is not kept) */
		if (!group_triangles[group] && !cutout_fill[group])
		{
			rt.body_counts[group][0] = rt.body_counts[group][1] = -1;
			continue;
		}
		geometry = [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
		geometry.vertexBuffer = rt.body_vertices[ring][group];
		geometry.vertexStride = 12;
		geometry.opaque = YES;
		cutouts = [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
		cutouts.vertexBuffer = rt.cut_vertices[ring][group];
		cutouts.vertexStride = 12;
		cutouts.opaque = NO;
		mesh = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
		mesh.usage = MTLAccelerationStructureUsageRefit;
		if (!rt.bodies[group])
		{
			/* sized for the most shapes, once (both kinds, full) */
			MTLAccelerationStructureSizes sizes;

			geometry.triangleCount = HOST_RT_GROUP_TRIANGLES;
			cutouts.triangleCount = HOST_RT_GROUP_TRIANGLES;
			mesh.geometryDescriptors = @[ geometry, cutouts ];
			sizes = [rt.device accelerationStructureSizesWithDescriptor:mesh];
			rt.bodies[group] = [rt.device newAccelerationStructureWithSize:sizes.accelerationStructureSize];
			rt.body_scratch[group] = [rt.device newBufferWithLength:MAX(sizes.buildScratchBufferSize,
				sizes.refitScratchBufferSize) options:MTLResourceStorageModePrivate];
			rt.body_counts[group][0] = rt.body_counts[group][1] = -1;
			if (!rt.bodies[group] || !rt.body_scratch[group])
			{
				[encoder endEncoding];
				return 0;
			}
		}
		geometry.triangleCount = (NSUInteger)group_triangles[group];
		cutouts.triangleCount = (NSUInteger)cutout_fill[group];
		if (group_triangles[group] && cutout_fill[group])
			mesh.geometryDescriptors = @[ geometry, cutouts ];
		else
			mesh.geometryDescriptors = @[ group_triangles[group] ? geometry : cutouts ];
		if (group_triangles[group] == rt.body_counts[group][0] && cutout_fill[group] == rt.body_counts[group][1] &&
			rt.body_age[group] < 30 && body_moved(ring, group, group_triangles[group], cutout_fill[group]) < 1.0f)
		{
			/* (the last frame's triangles: kept; moved a little - a
			character's, animated: refitted. Moved more, or others in their
			place - a full group's, as the camera goes: built anew, a refit's
			boxes would span the level) */
			if (body_moved(ring, group, group_triangles[group], cutout_fill[group]) == 0.0f)
				rt.body_age[group] = 0;
			else
			{
				[encoder refitAccelerationStructure:rt.bodies[group] descriptor:mesh destination:nil
					scratchBuffer:rt.body_scratch[group] scratchBufferOffset:0];
				rt.body_age[group]++;
			}
		}
		else
		{
			[encoder buildAccelerationStructure:rt.bodies[group] descriptor:mesh scratchBuffer:rt.body_scratch[group]
				scratchBufferOffset:0];
			rt.body_counts[group][0] = group_triangles[group];
			rt.body_counts[group][1] = cutout_fill[group];
			rt.body_age[group] = 0;
		}
		offsets[count] = (uint32_t)cutout_offsets[group];
		((uint32_t *)rt.instance_tris[ring].contents)[count * 4] = rt.tri_base[group][0];
		((uint32_t *)rt.instance_tris[ring].contents)[count * 4 + 1] = rt.tri_base[group][1];
		((uint32_t *)rt.instance_tris[ring].contents)[count * 4 + 2] = group_triangles[group] > 0 ? 1u : 0u;
		instances[count].mask = group_masks[group];
		if (rt.objects_two_sided)
			instances[count].options |= MTLAccelerationStructureInstanceOptionDisableTriangleCulling;
		instances[count].accelerationStructureIndex = (uint32_t)structures.count;
		[structures addObject:rt.bodies[group]];
		count++;
	}
	/* (after the meshes: a new pass sees them built) */
	[encoder endEncoding];
	descriptor = [MTLInstanceAccelerationStructureDescriptor descriptor];
	descriptor.instancedAccelerationStructures = structures;
	descriptor.instanceCount = (NSUInteger)count;
	descriptor.instanceDescriptorBuffer = buffer;
	descriptor.instanceDescriptorType = MTLAccelerationStructureInstanceDescriptorTypeDefault;
	if (!rt.scene)
	{
		MTLAccelerationStructureSizes sizes;

		descriptor.instanceCount = HOST_RT_GROUPS + 1;
		sizes = [rt.device accelerationStructureSizesWithDescriptor:descriptor];
		descriptor.instanceCount = (NSUInteger)count;
		rt.scene = [rt.device newAccelerationStructureWithSize:sizes.accelerationStructureSize];
		rt.scene_scratch = [rt.device newBufferWithLength:sizes.buildScratchBufferSize
			options:MTLResourceStorageModePrivate];
		if (!rt.scene || !rt.scene_scratch)
			return 0;
	}
	encoder = [commands accelerationStructureCommandEncoder];
	[encoder buildAccelerationStructure:rt.scene descriptor:descriptor scratchBuffer:rt.scene_scratch
		scratchBufferOffset:0];
	[encoder endEncoding];
	rt.scene_structures = structures;
	/* each object's bounding sphere, for the rays that can find only the
	level */
	rt.sphere_count = 0;
	for (group = 0; group < HOST_RT_GROUPS; group++)
	{
		const float *vertices = (const float *)rt.body_vertices[ring][group].contents;
		const float *cut = (const float *)rt.cut_vertices[ring][group].contents;
		float low[3] = { 1e30f, 1e30f, 1e30f }, high[3] = { -1e30f, -1e30f, -1e30f };
		int vertex, axis;

		if (!group_triangles[group] && !cutout_fill[group])
			continue;
		for (vertex = 0; vertex < (group_triangles[group] + cutout_fill[group]) * 3; vertex++)
		{
			for (axis = 0; axis < 3; axis++)
			{
				float value = vertex < group_triangles[group] * 3 ? vertices[vertex * 3 + axis] :
					cut[(vertex - group_triangles[group] * 3) * 3 + axis];

				if (value < low[axis])
					low[axis] = value;
				if (value > high[axis])
					high[axis] = value;
			}
		}
		for (axis = 0; axis < 3; axis++)
			rt.spheres[rt.sphere_count][axis] = (low[axis] + high[axis]) * 0.5f;
		rt.spheres[rt.sphere_count][3] = 0.5f * sqrtf((high[0] - low[0]) * (high[0] - low[0]) +
			(high[1] - low[1]) * (high[1] - low[1]) + (high[2] - low[2]) * (high[2] - low[2]));
		rt.sphere_count++;
	}
	return 1;
}

/* the GL texture sharing Metal texture `which` (0: the depth and normals,
32-bit floats; 1: the results, 16-bit floats), at this size */
uint32_t host_rt_texture(int which, int width, int height)
{
	MTLTextureDescriptor *descriptor;
	const EGLint_ attributes[] = { EGL_NONE_ };

	if (!rt.available || which < 0 || which > 3 || width <= 0 || height <= 0)
		return 0;
	if (rt.textures[which] && rt.widths[which] == width && rt.heights[which] == height)
		return rt.gl_textures[which];
	if (rt.gl_textures[which])
	{
		rt.glDeleteTextures(1, &rt.gl_textures[which]);
		rt.gl_textures[which] = 0;
	}
	if (rt.images[which] && rt.eglDestroyImageKHR)
		rt.eglDestroyImageKHR(rt.display, rt.images[which]);
	rt.images[which] = NULL;
	descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:which == 0 ? MTLPixelFormatRGBA32Float :
		MTLPixelFormatRGBA16Float width:(NSUInteger)width height:(NSUInteger)height mipmapped:NO];
	descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;
	descriptor.storageMode = MTLStorageModePrivate;
	rt.textures[which] = [rt.device newTextureWithDescriptor:descriptor];
	rt.images[which] = rt.textures[which] ? rt.eglCreateImageKHR(rt.display, EGL_NO_CONTEXT_, EGL_METAL_TEXTURE_ANGLE_,
		(__bridge void *)rt.textures[which], attributes) : NULL;
	if (!rt.images[which])
	{
		host_logf(HOST_LOG_ERROR, "ray tracing: ANGLE does not take the Metal texture");
		rt.textures[which] = nil;
		rt.available = 0;
		return 0;
	}
	rt.glGenTextures(1, &rt.gl_textures[which]);
	rt.glBindTexture(GL_TEXTURE_2D_, rt.gl_textures[which]);
	rt.glEGLImageTargetTexture2DOES(GL_TEXTURE_2D_, rt.images[which]);
	rt.glTexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MIN_FILTER_, GL_NEAREST_);
	rt.glTexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MAG_FILTER_, GL_NEAREST_);
	rt.widths[which] = width;
	rt.heights[which] = height;
	return rt.gl_textures[which];
}

/* the rays, for the camera (the 36 values the kernel names); 1 if done */
/* the time waited on GL (glFinish) and on the rays, for the frame
statistics (host_sdl.c) */
uint64_t host_rt_finish_ns, host_rt_trace_ns, host_rt_traces;

int host_rt_trace(const float *camera, int width, int height)
{
	uint64_t start, finished;
	id<MTLCommandBuffer> commands;
	id<MTLComputeCommandEncoder> encoder;
	MTLSize group, groups;

	rt.use_drawn = camera[46] > 0.5f;
	if (!rt.available || !level_structure() || !rt.textures[0] || !rt.textures[1] || !rt.textures[2] ||
		rt.widths[0] != width || rt.widths[1] != width || rt.heights[0] != height || rt.heights[1] != height ||
		rt.widths[2] != width || rt.heights[2] != height)
	{
		return 0;
	}
	/* the governor (the last trace's time, a frame or so behind - the rays'
	own, not the wait for GL): over 22 ms it steps up, under 16 ms for a
	second it steps back - the first two steps thin the traced light's new
	samples (every 8th frame a pixel, then 16th), the rest halve the lights
	and the emitters; a quarter of a second, ten frames in a row, and the
	rays stop for good */
	{
		double ms = rt.gpu_ms;

		if (ms > 250.0)
		{
			if (++rt.overloaded >= 10)
			{
				host_logf(HOST_LOG_ERROR, "ray tracing: %.0f ms a frame on the GPU; the rays stop", ms);
				rt.available = 0;
				return 0;
			}
		}
		else
			rt.overloaded = 0;
		if (ms > 22.0 && rt.shed < 7)
		{
			rt.shed++;
			rt.calm = 0;
			rt.gpu_ms = 0.0;
		}
		else if (ms < 16.0 && rt.shed > 0 && ++rt.calm >= 60)
		{
			rt.shed--;
			rt.calm = 0;
		}
	}
	start = SDL_GetTicksNS();
	if (rt.event)
	{
		/* GL signals the event when it has drawn the depth and normals */
		uint64_t drawn = ++rt.event_value;
		const EGLAttrib_ attributes[] = { EGL_SYNC_METAL_SHARED_EVENT_OBJECT_ANGLE_, (EGLAttrib_)(__bridge void *)rt.event,
			EGL_SYNC_METAL_SHARED_EVENT_SIGNAL_VALUE_LO_ANGLE_, (EGLAttrib_)(drawn & 0xFFFFFFFFu),
			EGL_SYNC_METAL_SHARED_EVENT_SIGNAL_VALUE_HI_ANGLE_, (EGLAttrib_)(drawn >> 32), EGL_NONE_ };
		void *sync = rt.eglCreateSync(rt.display, EGL_SYNC_METAL_SHARED_EVENT_ANGLE_, attributes);

		if (!sync)
		{
			host_logf(HOST_LOG_ERROR, "ray tracing: GL does not signal the Metal event; the CPU waits instead");
			rt.event = nil;
			rt.glFinish();
		}
		else
		{
			rt.eglDestroySync(rt.display, sync);
			rt.glFlush();
		}
	}
	else
	{
		rt.glFinish();
	}
	finished = SDL_GetTicksNS();
	/* (the wait for GL in a command buffer of its own: the rays' time on the
	GPU, which the governor reads, is theirs alone) */
	if (rt.event)
	{
		id<MTLCommandBuffer> wait = [rt.queue commandBuffer];

		[wait encodeWaitForEvent:rt.event value:rt.event_value];
		[wait commit];
	}
	/* (the objects' shapes built in a command buffer of their own: their
	time on the GPU, and the rays', logged apart) */
	{
		id<MTLCommandBuffer> builds = [rt.queue commandBuffer];

		if (!encode_scene(builds))
		{
			[builds commit];
			return 0;
		}
		[builds addCompletedHandler:^(id<MTLCommandBuffer> done) {
			if (done.GPUEndTime > done.GPUStartTime)
				rt.build_ms = (done.GPUEndTime - done.GPUStartTime) * 1000.0;
		}];
		[builds commit];
	}
	commands = [rt.queue commandBuffer];
	encoder = [commands computeCommandEncoder];
	[encoder setComputePipelineState:rt.pipeline];
	[encoder setTexture:rt.textures[0] atIndex:0];
	[encoder setTexture:rt.textures[1] atIndex:1];
	[encoder setTexture:rt.textures[2] atIndex:2];
	[encoder setAccelerationStructure:rt.scene atBufferIndex:0];
	for (id<MTLAccelerationStructure> structure in rt.scene_structures)
		[encoder useResource:structure usage:MTLResourceUsageRead];
	[encoder setAccelerationStructure:level_structure() atBufferIndex:2];
	[encoder setBytes:rt.spheres length:sizeof(rt.spheres) atIndex:3];
	[encoder setBytes:&rt.sphere_count length:sizeof(rt.sphere_count) atIndex:4];
	if (!rt.probe)
	{
		rt.probe = [rt.device newBufferWithLength:32 * 4 * sizeof(float) options:MTLResourceStorageModeShared];
		memset(rt.probe.contents, 0, rt.probe.length);
	}
	[encoder setBuffer:rt.probe offset:0 atIndex:5];
	[encoder setBytes:rt.lights length:sizeof(rt.lights) atIndex:6];
	{
		/* (the traced light's samples thin first - its period, below - then
		the lights and the emitters halve) */
		int cut = rt.shed > 2 ? rt.shed - 2 : 0;
		unsigned int light_count = rt.light_count >> cut, emitter_count = rt.emitter_count >> cut;

		/* (the 4 nearest always: the flashlight, what is at hand) */
		light_count = MAX(light_count, MIN(rt.light_count, 4u));
		emitter_count = MAX(emitter_count, MIN(rt.emitter_count, 4u));

		[encoder setBytes:&light_count length:sizeof(light_count) atIndex:7];
		[encoder setBytes:rt.emitters length:sizeof(rt.emitters) atIndex:8];
		[encoder setBytes:&emitter_count length:sizeof(emitter_count) atIndex:9];
	}
	/* the drawn level's surfaces and the traced light's textures (in their
	place, anything bound: the kernel reads them only when gi_ready) */
	{
		float constants[96] = { 0 };
		uint32_t gi_ready = camera[42] > 0.5f && rt.use_drawn && rt.drawn && rt.drawn_materials && rt.atlas;
		id<MTLBuffer> any = rt.probe;

		memcpy(constants, camera, 84 * sizeof(float));
		/* (the settings' bounces and rays a pixel, 92 and 93) */
		constants[92] = camera[92];
		constants[93] = camera[93];
		/* (the exposure: HALO_RT_EXPOSURE, a number, holds it) */
		if (!(rt.exposure > 0.0f))
			rt.exposure = 1.0f;
		constants[84] = getenv("HALO_RT_EXPOSURE") ? (float)atof(getenv("HALO_RT_EXPOSURE")) : rt.exposure;
		/* (the white balance: HALO_RT_WHITE_BALANCE=0 holds it at 1) */
		if (!(rt.balance[0] > 0.0f))
			rt.balance[0] = rt.balance[1] = rt.balance[2] = 1.0f;
		{
			int k, balanced = !getenv("HALO_RT_WHITE_BALANCE") || atoi(getenv("HALO_RT_WHITE_BALANCE")) != 0;

			for (k = 0; k < 3; k++)
				constants[88 + k] = balanced ? rt.balance[k] : 1.0f;
		}
		/* (the traced light's new samples: every 4th frame a pixel, every
		8th or 16th as the governor sheds) */
		if (!(camera[63] > 0.0f))
			constants[63] = (float)(4 << (rt.shed < 2 ? rt.shed : 2));
		/* (the path tracer's bounces: 3, 2 once the governor sheds, 1 from
		its third step) */
		{
			/* (the bounces: the setting's, 1 to 4; one fewer once the governor
			sheds, one only from its third step) */
			float bounces = camera[92] >= 1.0f ? (camera[92] > 4.0f ? 4.0f : camera[92]) : 3.0f;

			constants[85] = rt.shed >= 3 ? 1.0f : rt.shed >= 1 && bounces > 1.0f ? bounces - 1.0f : bounces;
		}
		/* (the most one ray to a glowing triangle brings: HALO_RT_GLOW_CLAMP) */
		constants[86] = getenv("HALO_RT_GLOW_CLAMP") ? (float)atof(getenv("HALO_RT_GLOW_CLAMP")) : 4.0f;
		if (gi_ready && (!rt.history[0] || rt.history[0].width != (NSUInteger)width ||
			rt.history[0].height != (NSUInteger)height))
		{
			MTLTextureDescriptor *descriptor = [MTLTextureDescriptor
				texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:(NSUInteger)width
				height:(NSUInteger)height mipmapped:NO];
			int index;

			descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
			descriptor.storageMode = MTLStorageModePrivate;
			for (index = 0; index < 2; index++)
				rt.history[index] = [rt.device newTextureWithDescriptor:descriptor];
			/* (the first frame has no history: its last camera is not taken) */
			constants[60] = 0.0f;
		}
		gi_ready = gi_ready && rt.history[0] && rt.history[1];
		/* (the drawn level's triangles and materials: the cutouts need them
		whatever the traced light, which the rest are for) */
		BOOL drawn_ready = rt.use_drawn && rt.drawn && rt.drawn_indices && rt.drawn_triangle_materials &&
			rt.drawn_materials;

		[encoder setBuffer:drawn_ready ? rt.drawn_indices : any offset:0 atIndex:10];
		[encoder setBuffer:gi_ready ? rt.drawn_texcoords : any offset:0 atIndex:11];
		[encoder setBuffer:drawn_ready ? rt.drawn_triangle_materials : any offset:0 atIndex:12];
		[encoder setBuffer:drawn_ready ? rt.drawn_materials : any offset:0 atIndex:13];
		[encoder setBuffer:gi_ready ? rt.drawn_pages : any offset:0 atIndex:14];
		[encoder setBytes:&gi_ready length:sizeof(gi_ready) atIndex:15];
		/* the cutouts: the level's coordinates, the masks, the objects' */
		{
			/* (the leaves' holes whatever the traced light: with it off, they
			were solid squares to the rays) */
			uint32_t cutouts_ready = drawn_ready && rt.mask_atlas && rt.mask_rects && rt.drawn_base_texcoords &&
				rt.object_cutouts[rt.scene_ring] && rt.instance_offsets[rt.scene_ring] && !getenv("HALO_RT_NO_CUTOUTS");

			[encoder setBuffer:cutouts_ready ? rt.drawn_base_texcoords : any offset:0 atIndex:21];
			[encoder setBuffer:cutouts_ready ? rt.mask_rects : any offset:0 atIndex:22];
			[encoder setBuffer:cutouts_ready ? rt.object_cutouts[rt.scene_ring] : any offset:0 atIndex:23];
			[encoder setBuffer:cutouts_ready ? rt.instance_offsets[rt.scene_ring] : any offset:0 atIndex:24];
			[encoder setBytes:&cutouts_ready length:sizeof(cutouts_ready) atIndex:25];
			[encoder setBuffer:gi_ready && rt.drawn_vertices ? rt.drawn_vertices : any offset:0 atIndex:26];
			/* (18 and 19: the probes' later, after the rays) */
			[encoder setBuffer:rt.object_tris[rt.scene_ring] ? rt.object_tris[rt.scene_ring] : any offset:0 atIndex:18];
			[encoder setBuffer:rt.instance_tris[rt.scene_ring] ? rt.instance_tris[rt.scene_ring] : any offset:0 atIndex:19];

			[encoder setTexture:cutouts_ready ? rt.mask_atlas : rt.textures[1] atIndex:7];
			constants[83] = (float)rt.drawn_cutout_start;
		}
		/* the exposure's sums, zeroed, read when the rays are done */
		{
			int ring = rt.exposure_ring;
			id<MTLBuffer> sums;

			if (!rt.exposure_sums[ring])
				rt.exposure_sums[ring] = [rt.device newBufferWithLength:64 options:MTLResourceStorageModeShared];
			sums = rt.exposure_sums[ring];
			memset(sums.contents, 0, 64);
			[encoder setBuffer:sums offset:0 atIndex:27];
			rt.exposure_ring = (ring + 1) % 3;
			if (gi_ready)
			{
				[commands addCompletedHandler:^(id<MTLCommandBuffer> done) {
					const uint32_t *values = sums.contents;

					(void)done;
					/* (the baked light over the traced, over a hundred pixels or
					more, followed a twentieth a frame, 0.25 to 8) */
					if (values[2] > 100 && values[1] > values[2])
					{
						float target = (float)values[0] / (float)values[1];

						target = target < 0.25f ? 0.25f : target > 8.0f ? 8.0f : target;
						rt.exposure *= powf(target / rt.exposure, 0.05f);
						/* (the white balance: each colour's baked over traced, over
						the brightness's - the lightmaps' tint, 0.5 to 2 -
						followed as slowly) */
						for (int k = 0; k < 3; k++)
						{
							float tint = values[6 + k] > 0 && values[0] > 0 ?
								((float)values[3 + k] / (float)values[6 + k]) / ((float)values[0] / (float)values[1]) : 1.0f;

							tint = tint < 0.5f ? 0.5f : tint > 2.0f ? 2.0f : tint;
							rt.balance[k] *= powf(tint / rt.balance[k], 0.05f);
						}
					}
				}];
			}
		}
		{
			uint32_t glowing_count = gi_ready && rt.glowing ? rt.glowing_count : 0;

			[encoder setBuffer:glowing_count ? rt.glowing : any offset:0 atIndex:16];
			[encoder setBytes:&glowing_count length:sizeof(glowing_count) atIndex:17];
			/* (the light grid, when it is of these glowing triangles) */
			@synchronized (rt.queue)
			{
				BOOL gridded = glowing_count && rt.grid_info && rt.grid_glowing == rt.glowing && !getenv("HALO_RT_NO_GRID");
				static const float none[8] = { 0 };

				if (gridded)
					[encoder setBuffer:rt.grid_info offset:0 atIndex:28];
				else
					[encoder setBytes:none length:sizeof(none) atIndex:28];
				[encoder setBuffer:gridded ? rt.grid_cells : any offset:0 atIndex:29];
				[encoder setBuffer:gridded ? rt.grid_entries : any offset:0 atIndex:30];
			}
		}
		[encoder setTexture:gi_ready ? rt.atlas : rt.textures[1] atIndex:3];
		[encoder setTexture:gi_ready ? rt.history[rt.history_index] : rt.textures[1] atIndex:5];
		[encoder setTexture:gi_ready ? rt.history[rt.history_index ^ 1] : rt.textures[2] atIndex:6];
		if (gi_ready)
			rt.history_index ^= 1;
		[encoder setBytes:constants length:sizeof(constants) atIndex:1];
	}
	[commands addCompletedHandler:^(id<MTLCommandBuffer> done) {
		if (done.GPUEndTime > done.GPUStartTime)
		{
			static double total, builds;
			static int frames;

			rt.gpu_ms = (done.GPUEndTime - done.GPUStartTime) * 1000.0 + rt.build_ms;
			builds += rt.build_ms;
			/* (every 600 frames: the rays' average time on the GPU) */
			total += rt.gpu_ms;
			if (++frames == (getenv("HALO_RT_LOG_FRAMES") ? atoi(getenv("HALO_RT_LOG_FRAMES")) : 600))
			{
				host_logf(HOST_LOG_INFO, "ray tracing: %.2f ms a frame on the GPU (the objects' shapes %.2f; shed %d, "
					"exposure %.2f, white balance %.2f %.2f %.2f)", total / frames, builds / frames, rt.shed, rt.exposure,
					rt.balance[0], rt.balance[1], rt.balance[2]);
				total = 0.0;
				builds = 0.0;
				frames = 0;
			}
		}
	}];
	group = MTLSizeMake(8, 8, 1);
	groups = MTLSizeMake(((NSUInteger)width + 7) / 8, ((NSUInteger)height + 7) / 8, 1);
	[encoder dispatchThreadgroups:groups threadsPerThreadgroup:group];
	/* the light probes asked for this frame, read back when the rays are
	done (the next frame's host_rt_probe_results) */
	if (rt.probe_pipeline && rt.probe_count > 0 && camera[42] > 0.5f && rt.use_drawn && rt.drawn &&
		rt.drawn_materials && rt.atlas)
	{
		int ring = rt.probe_ring;
		uint32_t count = rt.probe_count;
		id<MTLBuffer> in, out;
		float *points;

		if (!rt.probe_in[ring])
		{
			rt.probe_in[ring] = [rt.device newBufferWithLength:64 * 16 options:MTLResourceStorageModeShared];
			rt.probe_out[ring] = [rt.device newBufferWithLength:64 * 32 options:MTLResourceStorageModeShared];
		}
		in = rt.probe_in[ring];
		out = rt.probe_out[ring];
		memcpy(in.contents, rt.probe_points, count * 16);
		[encoder setComputePipelineState:rt.probe_pipeline];
		[encoder setBuffer:in offset:0 atIndex:18];
		[encoder setBuffer:out offset:0 atIndex:19];
		[encoder setBytes:&count length:sizeof(count) atIndex:20];
		[encoder dispatchThreads:MTLSizeMake(count, 1, 1) threadsPerThreadgroup:MTLSizeMake(count < 32 ? count : 32, 1, 1)];
		rt.probe_ring = (ring + 1) % 3;
		points = malloc(count * 16);
		if (points)
		{
			memcpy(points, rt.probe_points, count * 16);
			[commands addCompletedHandler:^(id<MTLCommandBuffer> done) {
				const float *values = out.contents;
				uint32_t index;

				(void)done;
				@synchronized (rt.queue)
				{
					for (index = 0; index < count; index++)
					{
						memcpy(rt.probe_results + index * 10, points + index * 4, 3 * sizeof(float));
						memcpy(rt.probe_results + index * 10 + 3, values + index * 8, 4 * sizeof(float));
						memcpy(rt.probe_results + index * 10 + 7, values + index * 8 + 4, 3 * sizeof(float));
					}
					rt.probe_result_count = (int)count;
				}
				free(points);
			}];
		}
	}
	rt.probe_count = 0;
	[encoder endEncoding];
	if (rt.event)
	{
		/* GL waits on the GPU for the rays to finish */
		uint64_t traced = ++rt.event_value;
		const EGLAttrib_ attributes[] = { EGL_SYNC_CONDITION_, EGL_SYNC_METAL_SHARED_EVENT_SIGNALED_ANGLE_,
			EGL_SYNC_METAL_SHARED_EVENT_OBJECT_ANGLE_, (EGLAttrib_)(__bridge void *)rt.event,
			EGL_SYNC_METAL_SHARED_EVENT_SIGNAL_VALUE_LO_ANGLE_, (EGLAttrib_)(traced & 0xFFFFFFFFu),
			EGL_SYNC_METAL_SHARED_EVENT_SIGNAL_VALUE_HI_ANGLE_, (EGLAttrib_)(traced >> 32), EGL_NONE_ };
		void *sync;

		[commands encodeSignalEvent:rt.event value:traced];
		[commands commit];
		sync = rt.eglCreateSync(rt.display, EGL_SYNC_METAL_SHARED_EVENT_ANGLE_, attributes);
		if (sync && rt.eglWaitSync(rt.display, sync, 0))
		{
			rt.eglDestroySync(rt.display, sync);
			host_rt_finish_ns += finished - start;
			host_rt_trace_ns += SDL_GetTicksNS() - finished;
			host_rt_traces++;
			return 1;
		}
		if (sync)
			rt.eglDestroySync(rt.display, sync);
		host_logf(HOST_LOG_ERROR, "ray tracing: GL does not wait for the Metal event; the CPU waits instead");
		rt.event = nil;
		[commands waitUntilCompleted];
		return commands.status == MTLCommandBufferStatusCompleted;
	}
	[commands commit];
	[commands waitUntilCompleted];
	host_rt_finish_ns += finished - start;
	host_rt_trace_ns += SDL_GetTicksNS() - finished;
	host_rt_traces++;
	return commands.status == MTLCommandBufferStatusCompleted;
}

/* this frame's dynamic lights, 12 floats each (the position, the radius, the
direction, the cosine of the cone's cutoff or -2 all round, how far short of
the light its rays stop) */
void host_rt_set_lights(const float *lights, int count)
{
	if (count < 0)
		count = 0;
	if (count > HOST_RT_MAXIMUM_LIGHTS)
		count = HOST_RT_MAXIMUM_LIGHTS;
	memcpy(rt.lights, lights, (size_t)count * 12 * sizeof(float));
	rt.light_count = (unsigned int)count;
}

/* this frame's emitters, 8 floats each (the position, the radius, the
colour, the intensity) */
void host_rt_set_emitters(const float *emitters, int count)
{
	if (count < 0)
		count = 0;
	if (count > HOST_RT_MAXIMUM_EMITTERS)
		count = HOST_RT_MAXIMUM_EMITTERS;
	memcpy(rt.emitters, emitters, (size_t)count * 8 * sizeof(float));
	rt.emitter_count = (unsigned int)count;
}

/* the ray probe's last rays: up to maximum segments of 8 floats each (from
x, y, z, kind: 0 occlusion, 1 the sun, 2 the reflection, 3 the normal; to
x, y, z, whether it hit); returns how many */
int host_rt_probe(float *segments, int maximum)
{
	const float *values;
	int count;

	if (!rt.available || !rt.probe || maximum <= 0)
		return 0;
	values = (const float *)rt.probe.contents;
	count = (int)values[0];
	if (count < 0)
		count = 0;
	if (count > 15)
		count = 15;
	if (count > maximum)
		count = maximum;
	memcpy(segments, values + 4, (size_t)count * 8 * sizeof(float));
	return count;
}

/* tests: count pixels of row y of shared texture `which` from x, as floats
(RGBA; the results texture's halves widened), into values */
int host_rt_debug_read(int which, int x, int y, int count, float *values)
{
	id<MTLBuffer> buffer;
	id<MTLCommandBuffer> commands;
	id<MTLBlitCommandEncoder> blit;
	int bytes = which == 0 ? 16 : 8, index;

	if (!rt.available || which < 0 || which > 1 || !rt.textures[which])
		return 0;
	rt.glFinish();
	buffer = [rt.device newBufferWithLength:(NSUInteger)(count * bytes) options:MTLResourceStorageModeShared];
	commands = [rt.queue commandBuffer];
	blit = [commands blitCommandEncoder];
	[blit copyFromTexture:rt.textures[which] sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake((NSUInteger)x,
		(NSUInteger)y, 0) sourceSize:MTLSizeMake((NSUInteger)count, 1, 1) toBuffer:buffer destinationOffset:0
		destinationBytesPerRow:(NSUInteger)(count * bytes) destinationBytesPerImage:(NSUInteger)(count * bytes)];
	[blit endEncoding];
	[commands commit];
	[commands waitUntilCompleted];
	for (index = 0; index < count * 4; index++)
		values[index] = which == 0 ? ((const float *)buffer.contents)[index] :
			(float)((const __fp16 *)buffer.contents)[index];
	return 1;
}
