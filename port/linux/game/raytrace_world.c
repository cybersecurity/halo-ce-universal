/*
RAYTRACE_WORLD.C

The level's geometry for the ray-traced lighting of the macOS port
(port/linux/src/raytrace_gl.c, port/macos/host/host_metal_rt.m): the active
structure BSP's collision surfaces as a triangle mesh in world units. The
collision surfaces are the level's solid shape - its floors, walls and
ceilings - without the detail of its rendered geometry; the invisible ones
(player clip) are left out.

Each surface is a convex polygon whose edges form a ring: an edge belongs
to two surfaces, and for each it names the next edge around it
(collision_edge.edge_indices, by the side the surface is on). The polygon
is split into a fan of triangles.
*/

#include "cseries.h"
#include <math.h>
#include "physics/collision_bsp_definitions.h"
#include "scenario/scenario.h"
#include "render/render.h"
#include "tag_files/tag_files.h"
#include "objects/objects.h"
#include "objects/object_types.h"
#include "objects/object_definitions.h"
#include "models/model_definitions.h"
#include "game/players.h"
#include "bitmaps/bitmap_group.h"
#include "bitmaps/bitmap_group_lookup.h"

enum
{
	_collision_surface_invisible_flag = 1 << 1,
};

static struct
{
	const struct collision_bsp *bsp;
	unsigned long generation;
	float *vertices;
	long vertex_count;
	unsigned long *indices;
	long triangle_count;
} world;

static void world_build(const struct collision_bsp *bsp)
{
	const struct collision_surface *surfaces = bsp->surfaces.address;
	const struct collision_edge *edges = bsp->edges.address;
	const struct collision_vertex *vertices = bsp->vertices.address;
	long surface_index, vertex_index, capacity;

	/* (the game's allocator refuses NULL) */
	if (world.vertices)
		free(world.vertices);
	if (world.indices)
		free(world.indices);
	world.vertices = NULL;
	world.indices = NULL;
	world.vertex_count = 0;
	world.triangle_count = 0;
	world.bsp = bsp;
	world.generation++;
	if (!bsp || bsp->vertices.count <= 0 || bsp->surfaces.count <= 0 || bsp->edges.count <= 0)
		return;

	world.vertices = malloc((size_t)bsp->vertices.count * 3 * sizeof(float));
	/* at most MAXIMUM_VERTICES_PER_COLLISION_SURFACE - 2 triangles each */
	capacity = bsp->surfaces.count * (MAXIMUM_VERTICES_PER_COLLISION_SURFACE - 2);
	world.indices = malloc((size_t)capacity * 3 * sizeof(unsigned long));
	if (!world.vertices || !world.indices)
		return;
	for (vertex_index = 0; vertex_index < bsp->vertices.count; vertex_index++)
	{
		world.vertices[vertex_index * 3 + 0] = vertices[vertex_index].point.x;
		world.vertices[vertex_index * 3 + 1] = vertices[vertex_index].point.y;
		world.vertices[vertex_index * 3 + 2] = vertices[vertex_index].point.z;
	}
	world.vertex_count = bsp->vertices.count;

	for (surface_index = 0; surface_index < bsp->surfaces.count; surface_index++)
	{
		const struct collision_surface *surface = &surfaces[surface_index];
		long ring[MAXIMUM_VERTICES_PER_COLLISION_SURFACE];
		float normal[3];
		long count = 0, edge_index = surface->first_edge_index, steps, corner;

		if (surface->flags & _collision_surface_invisible_flag)
			continue;
		/* the ring, guarded against a malformed one */
		for (steps = 0; steps < MAXIMUM_EDGES_PER_COLLISION_SURFACE * 2; steps++)
		{
			const struct collision_edge *edge;
			int side;

			if (edge_index < 0 || edge_index >= bsp->edges.count)
				break;
			edge = &edges[edge_index];
			side = edge->surface_indices[0] == surface_index ? 0 : 1;
			if (count < MAXIMUM_VERTICES_PER_COLLISION_SURFACE)
				ring[count++] = edge->vertex_indices[side];
			edge_index = edge->edge_indices[side];
			if (edge_index == surface->first_edge_index)
				break;
		}
		/* the surface's outward normal: its plane, negated by the
		designator's sign bit */
		{
			long plane_index = surface->plane_designator & LONG_MAX;

			if (plane_index < bsp->bsp3d.planes.count)
			{
				const real_plane3d *plane = (const real_plane3d *)bsp->bsp3d.planes.address + plane_index;
				float sign = (surface->plane_designator & LONG_MIN) ? -1.0f : 1.0f;

				normal[0] = plane->n.i * sign;
				normal[1] = plane->n.j * sign;
				normal[2] = plane->n.k * sign;
			}
			else
			{
				normal[0] = normal[1] = normal[2] = 0.0f;
			}
		}
		for (corner = 1; corner + 1 < count && world.triangle_count < capacity; corner++)
		{
			long a = ring[0], b = ring[corner], c = ring[corner + 1];

			if (a < 0 || b < 0 || c < 0 || a >= world.vertex_count || b >= world.vertex_count ||
				c >= world.vertex_count)
			{
				continue;
			}
			/* wound counterclockwise seen from the outside, so the rays can
			pass out of the level's surfaces from behind them: where the
			rendered surface lies behind its collision surface, a ray
			leaving it does not find the collision surface's back */
			{
				const float *pa = &world.vertices[a * 3], *pb = &world.vertices[b * 3], *pc = &world.vertices[c * 3];
				float u[3] = { pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2] };
				float v[3] = { pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2] };
				float facing = (u[1] * v[2] - u[2] * v[1]) * normal[0] + (u[2] * v[0] - u[0] * v[2]) * normal[1] +
					(u[0] * v[1] - u[1] * v[0]) * normal[2];

				if (facing < 0.0f)
				{
					long swap = b;

					b = c;
					c = swap;
				}
			}
			world.indices[world.triangle_count * 3 + 0] = (unsigned long)a;
			world.indices[world.triangle_count * 3 + 1] = (unsigned long)b;
			world.indices[world.triangle_count * 3 + 2] = (unsigned long)c;
			world.triangle_count++;
		}
	}
}

/* the active BSP's mesh: returns its generation, which changes when the
BSP does; 0 while there is none */
unsigned long halo_ray_tracing_world(const float **vertices, long *vertex_count, const unsigned long **indices,
	long *triangle_count)
{
	const struct collision_bsp *bsp = global_structure_bsp_index != NONE ? global_collision_bsp : NULL;

	if (bsp != world.bsp)
		world_build(bsp);
	if (!bsp || !world.triangle_count)
		return 0;
	*vertices = world.vertices;
	*vertex_count = world.vertex_count;
	*indices = world.indices;
	*triangle_count = world.triangle_count;
	return world.generation;
}

/* ---------- the sun

The first light of the visible sky with a direction (the one whose lens
flare the sky draws: render_sky.c) is taken as the sun. */


struct sky_light_view
{
	struct tag_reference lens_flare;
	char marker_name[TAG_STRING_LENGTH + 1];
	byte pad31[0x37];
	real_euler_angles2d direction;
	byte pad70[4];
};

struct sky_view
{
	struct tag_reference model;
	struct tag_reference animation_graph;
	byte pad20[0x8C];
	struct tag_block render_model_regions;
	struct tag_block animations;
	struct tag_block lights;
};

typedef char sky_light_view_size_assert[sizeof(struct sky_light_view) == 0x74 ? 1 : -1];
typedef char sky_view_lights_offset_assert[offsetof(struct sky_view, lights) == 0xC4 ? 1 : -1];

struct sky *scenario_get_sky(short sky_index);
void platform_log(const char *format, ...);

/* the direction towards the sun in the world (unit length); FALSE if the
visible sky has none */
boolean halo_ray_tracing_sun(float *direction)
{
	const struct sky_view *sky;
	long index;

	if (render.visible_sky_index == NONE)
		return FALSE;
	sky = (const struct sky_view *)scenario_get_sky(render.visible_sky_index);
	if (!sky)
		return FALSE;
	for (index = 0; index < sky->lights.count; index++)
	{
		const struct sky_light_view *light = (const struct sky_light_view *)sky->lights.address + index;
		real_vector3d vector;

		if (light->lens_flare.index == NONE)
			continue;
		vector3d_from_euler_angles2d(&vector, &light->direction);
		direction[0] = vector.i;
		direction[1] = vector.j;
		direction[2] = vector.k;
		return TRUE;
	}
	return FALSE;
}

/* ---------- the objects

The units (the bipeds and the vehicles) as triangles for the rays, in the
world, each frame: their drawn models, skinned as the renderer skins them
(display.ray_tracing_shapes "model", at the high level of detail), or
their collision models - the meshes the game tests its
bullets against, a mesh for each node's region as it now is (its damage
permutation), placed by the node's matrix as the animation poses it (between
the last two ticks, as the frame draws it). One without either is left
out. Each triangle's group: its
object (0 to 31, the player's first) above, its kind below (2 an object, 4
the local player's body: the first person does not draw it, so only the
rays show its shadow). */

#include "physics/collision_model_definitions.h"

enum
{
	_ray_mask_object = 2,
	_ray_mask_player = 4,
};

/* the objects nearer the camera than this (world units) are in the rays */
#define RAY_TRACED_OBJECT_DISTANCE 25.0f
/* how far the camera goes before the objects near it are chosen again */
#define RAY_TRACED_OBJECT_ANCHOR_STEP 4.0f
static const float *ray_object_anchor;
#define RAY_TRACED_OBJECT_GROUPS 32

/* the model geometry's layout (models.c keeps it private) */
struct model_geometry_view
{
	byte reserved[0x24];
	struct tag_block parts;
};

struct model_geometry_part_view
{
	unsigned long flags;
	short shader_index;
	char previous_part_index;
	char next_part_index;
	short centroid_primary_node_index;
	short centroid_secondary_node_index;
	real centroid_primary_node_weight;
	real centroid_secondary_node_weight;
	real_point3d centroid;
	struct tag_block uncompressed_vertices;
	struct tag_block compressed_vertices;
	struct tag_block triangles;
	struct
	{
		short type;
		word pad;
		long count;
		void *base_address;
		void *hardware_format;
	} triangle_buffer;
	struct
	{
		short type;
		word pad;
		long count;
		long offset;
		void *base_address;
		void *hardware_format;
	} vertex_buffer;
};

/* a compressed model vertex, as the renderer reads it (rasterizer.c) */
struct model_vertex_view
{
	real_point3d position;
	unsigned long normal;
	unsigned long binormal;
	unsigned long tangent;
	short texture_coordinates[2];
	char node_indices[2];
	short node_weight;
};

typedef char model_geometry_part_view_size[sizeof(struct model_geometry_part_view) == 0x68 ? 1 : -1];
typedef char model_vertex_view_size[sizeof(struct model_vertex_view) == 32 ? 1 : -1];

/* a collision mesh's triangles, in its node's space (9 floats each), made
once for each mesh while its map is loaded */
struct mesh_triangles
{
	const struct collision_bsp *bsp;
	unsigned long generation;
	float *triangles;
	long count;
};

static struct mesh_triangles meshes[512];
static long mesh_count;

static long bsp_triangulate(const struct collision_bsp *bsp, float *out, long maximum)
{
	const struct collision_surface *surfaces = bsp->surfaces.address;
	const struct collision_edge *edges = bsp->edges.address;
	const struct collision_vertex *vertices = bsp->vertices.address;
	long surface_index, count = 0;

	for (surface_index = 0; surface_index < bsp->surfaces.count && count < maximum; surface_index++)
	{
		const struct collision_surface *surface = &surfaces[surface_index];
		long ring[MAXIMUM_VERTICES_PER_COLLISION_SURFACE];
		long ring_count = 0, edge_index = surface->first_edge_index, steps, corner, plane_index;
		float normal[3] = { 0.0f, 0.0f, 0.0f };

		for (steps = 0; steps < MAXIMUM_EDGES_PER_COLLISION_SURFACE * 2; steps++)
		{
			const struct collision_edge *edge;
			int side;

			if (edge_index < 0 || edge_index >= bsp->edges.count)
				break;
			edge = &edges[edge_index];
			side = edge->surface_indices[0] == surface_index ? 0 : 1;
			if (ring_count < MAXIMUM_VERTICES_PER_COLLISION_SURFACE)
				ring[ring_count++] = edge->vertex_indices[side];
			edge_index = edge->edge_indices[side];
			if (edge_index == surface->first_edge_index)
				break;
		}
		plane_index = surface->plane_designator & LONG_MAX;
		if (plane_index < bsp->bsp3d.planes.count)
		{
			const real_plane3d *plane = (const real_plane3d *)bsp->bsp3d.planes.address + plane_index;
			float sign = (surface->plane_designator & LONG_MIN) ? -1.0f : 1.0f;

			normal[0] = plane->n.i * sign;
			normal[1] = plane->n.j * sign;
			normal[2] = plane->n.k * sign;
		}
		for (corner = 1; corner + 1 < ring_count && count < maximum; corner++)
		{
			long a = ring[0], b = ring[corner], c = ring[corner + 1];
			const float *pa, *pb, *pc;
			float u[3], v[3], facing, *t = out + count * 9;

			if (a < 0 || b < 0 || c < 0 || a >= bsp->vertices.count || b >= bsp->vertices.count ||
				c >= bsp->vertices.count)
			{
				continue;
			}
			pa = &vertices[a].point.x;
			pb = &vertices[b].point.x;
			pc = &vertices[c].point.x;
			/* wound counterclockwise seen from outside, as the level's */
			u[0] = pb[0] - pa[0];
			u[1] = pb[1] - pa[1];
			u[2] = pb[2] - pa[2];
			v[0] = pc[0] - pa[0];
			v[1] = pc[1] - pa[1];
			v[2] = pc[2] - pa[2];
			facing = (u[1] * v[2] - u[2] * v[1]) * normal[0] + (u[2] * v[0] - u[0] * v[2]) * normal[1] +
				(u[0] * v[1] - u[1] * v[0]) * normal[2];
			if (facing < 0.0f)
			{
				const float *swap = pb;

				pb = pc;
				pc = swap;
			}
			memcpy(t, pa, 3 * sizeof(float));
			memcpy(t + 3, pb, 3 * sizeof(float));
			memcpy(t + 6, pc, 3 * sizeof(float));
			count++;
		}
	}
	return count;
}

/* the mesh's triangles, made the first time it is asked for; NULL if none */
static const struct mesh_triangles *mesh_get(const struct collision_bsp *bsp)
{
	long index, capacity;
	struct mesh_triangles *mesh;

	for (index = 0; index < mesh_count; index++)
	{
		if (meshes[index].bsp == bsp && meshes[index].generation == world.generation)
			return meshes[index].count > 0 ? &meshes[index] : NULL;
	}
	/* a new map: the meshes of the last are gone */
	if (mesh_count && meshes[0].generation != world.generation)
	{
		for (index = 0; index < mesh_count; index++)
		{
			if (meshes[index].triangles)
				free(meshes[index].triangles);
		}
		mesh_count = 0;
	}
	if (mesh_count >= (long)(sizeof(meshes) / sizeof(meshes[0])))
		return NULL;
	mesh = &meshes[mesh_count++];
	mesh->bsp = bsp;
	mesh->generation = world.generation;
	mesh->triangles = NULL;
	mesh->count = 0;
	capacity = bsp->surfaces.count * (MAXIMUM_VERTICES_PER_COLLISION_SURFACE - 2);
	if (capacity <= 0)
		return NULL;
	mesh->triangles = malloc((size_t)capacity * 9 * sizeof(float));
	if (!mesh->triangles)
		return NULL;
	mesh->count = bsp_triangulate(bsp, mesh->triangles, capacity);
	return mesh->count > 0 ? mesh : NULL;
}

/* the shapes for the rays: the drawn models or the collision models
(display.ray_tracing_shapes) */
enum
{
	_ray_shapes_model,
	_ray_shapes_collision,
};

/* the model's level of detail in the rays: high (of super low to super high) */
#define RAY_TRACED_MODEL_DETAIL_LEVEL 3
#define RAY_TRACED_MAXIMUM_NODES 128

/* the object's drawn model, skinned as the renderer skins it (each vertex
by its two nodes' matrices, each the node's pose times its inverse default
pose: models.c), its regions as they are now; returns how many triangles */
/* this frame's objects' triangles and their cutouts (8 floats each: the
corners' texture coordinates, the mask or -1, 0), for the triangles'
indices from the first */
static const float *ray_triangles_base;
static float *ray_cutouts;
static long mask_index(struct bitmap_data *bitmap);

static float *triangle_cutout(const float *triangle)
{
	return ray_cutouts && ray_triangles_base ? ray_cutouts + (triangle - ray_triangles_base) / 9 * 8 : NULL;
}

static boolean level_bitmap_readable(struct bitmap_data *bitmap);
static void level_bitmap_average(struct bitmap_data *bitmap, float *average);

/* a model part's colour in the rays: its base map's average, 8 bits each of
red, green and blue in a float's bits (0: not read yet - the rays take a
mid grey), read once for each bitmap as the texture cache has it */
static float object_albedo(struct bitmap_data *bitmap)
{
	static struct
	{
		struct bitmap_data *bitmap;
		unsigned long packed;
	} known[512];
	unsigned long slot = ((unsigned long)bitmap >> 4) % 512, tries;
	float average[3], result;

	if (!bitmap)
		return 0.0f;
	for (tries = 0; tries < 8; tries++, slot = (slot + 1) % 512)
	{
		if (known[slot].bitmap == bitmap)
		{
			memcpy(&result, &known[slot].packed, sizeof(result));
			return result;
		}
		if (!known[slot].bitmap)
			break;
	}
	if (tries == 8 || !level_bitmap_readable(bitmap))
		return 0.0f;
	level_bitmap_average(bitmap, average);
	known[slot].bitmap = bitmap;
	known[slot].packed = 0x01000000UL | ((unsigned long)(PIN(average[0], 0.0f, 1.0f) * 255.0f) << 16) |
		((unsigned long)(PIN(average[1], 0.0f, 1.0f) * 255.0f) << 8) | (unsigned long)(PIN(average[2], 0.0f, 1.0f) * 255.0f);
	memcpy(&result, &known[slot].packed, sizeof(result));
	return result;
}

static long model_triangles(struct object_datum *object, const real_matrix4x3 *matrices, float *out, long room,
	const real_matrix4x3 *rigid)
{
	const struct object_definition *definition = object_definition_get(object->definition_index);
	const struct model *model;
	const struct model_node *nodes;
	static real_matrix4x3 relative[RAY_TRACED_MAXIMUM_NODES];
	long count = 0, node_index, region_index, part_mask = -1;
	float u_scale = 1.0f, v_scale = 1.0f, part_albedo = 0.0f;

	if (!matrices || definition->object.model.index == NONE)
		return 0;
	model = model_definition_get(definition->object.model.index);
	if (model->nodes.count <= 0 || model->nodes.count > RAY_TRACED_MAXIMUM_NODES)
		return 0;
	nodes = (const struct model_node *)model->nodes.address;
	/* (each node's pose over its rest pose; or, rigid, the model at rest,
	placed where the object is) */
	for (node_index = 0; node_index < model->nodes.count; node_index++)
	{
		if (rigid)
			relative[node_index] = *rigid;
		else
			matrix4x3_multiply(&matrices[node_index], &nodes[node_index].runtime_default_inverse_matrix,
				&relative[node_index]);
	}
	for (region_index = 0; region_index < model->regions.count && count < room; region_index++)
	{
		const struct model_region *region = (const struct model_region *)model->regions.address + region_index;
		const struct model_region_permutation *permutation;
		const struct model_geometry_view *geometry;
		char permutation_index = object->object.region_permutations[region_index];
		short geometry_index, part_index;

		if (permutation_index == NONE || permutation_index >= region->permutations.count)
			continue;
		permutation = (const struct model_region_permutation *)region->permutations.address + permutation_index;
		/* (high, or the nearest level of detail the model has) */
		{
			short level;

			geometry_index = NONE;
			for (level = RAY_TRACED_MODEL_DETAIL_LEVEL; level >= 0 && geometry_index == NONE; level--)
				geometry_index = permutation->geometry_indices[level];
			for (level = RAY_TRACED_MODEL_DETAIL_LEVEL + 1; level < 5 && geometry_index == NONE; level++)
				geometry_index = permutation->geometry_indices[level];
		}
		if (geometry_index == NONE || geometry_index >= model->geometries.count)
			continue;
		geometry = (const struct model_geometry_view *)model->geometries.address + geometry_index;
		for (part_index = 0; part_index < geometry->parts.count && count < room; part_index++)
		{
			const struct model_geometry_part_view *part = (const struct model_geometry_part_view *)geometry->parts.address +
				part_index;
			/* the part's strip and vertices: in a cache file, not in the tag
			blocks but in its buffers - the strip in memory, the vertices at a
			physical address, which the CPU sees in the window at 0x80000000 */
			const unsigned short *strip = (const unsigned short *)part->triangle_buffer.base_address;
			unsigned long vertex_address = (unsigned long)part->vertex_buffer.base_address;
			const byte *vertex_data;
			boolean compressed = part->vertex_buffer.type == 5;
			long index, strip_count = part->triangle_buffer.count + 2, vertex_count = part->vertex_buffer.count;
			long vertex_size = compressed ? (long)sizeof(struct model_vertex_view) : 68;

			/* (where the GPU reads them: the vertex buffer resource's data -
			Common, Data, Lock - a physical address; base_address is where the
			cache file's vertices were read to, whose memory the game reuses) */
			if (part->vertex_buffer.hardware_format && ((const unsigned long *)part->vertex_buffer.hardware_format)[1])
				vertex_address = ((const unsigned long *)part->vertex_buffer.hardware_format)[1];
			if (vertex_address && vertex_address < 0x80000000UL)
				vertex_address |= 0x80000000UL;
			vertex_data = (const byte *)vertex_address + part->vertex_buffer.offset * vertex_size;
			/* (or the tag blocks, where they are kept) */
			if (!strip || !vertex_address)
			{
				strip = (const unsigned short *)part->triangles.address;
				vertex_data = (const byte *)part->compressed_vertices.address;
				vertex_count = part->compressed_vertices.count;
				compressed = TRUE;
				vertex_size = (long)sizeof(struct model_vertex_view);
			}

			/* (HALO_RT_LOG_SHAPES: each model's parts' data, once) */
			if (getenv("HALO_RT_LOG_SHAPES") && part_index == 0 && region_index == 0)
			{
				extern void platform_log(const char *format, ...);
				static long logged[64];
				static int logged_count;
				int seen = 0, k;

				for (k = 0; k < logged_count; k++)
					seen |= logged[k] == definition->object.model.index;
				if (!seen && logged_count < 64)
				{
					logged[logged_count++] = definition->object.model.index;
					platform_log("ray tracing model %ld: part flags %lx, triangles block %ld at %p, compressed %ld at %p, "
						"uncompressed %ld at %p, triangle buffer type %d count %ld at %p, vertex buffer type %d count %ld "
						"offset %ld at %p", definition->object.model.index, (unsigned long)part->flags,
						(long)part->triangles.count, part->triangles.address, (long)part->compressed_vertices.count,
						part->compressed_vertices.address, (long)part->uncompressed_vertices.count,
						part->uncompressed_vertices.address, part->triangle_buffer.type, part->triangle_buffer.count,
						part->triangle_buffer.base_address, part->vertex_buffer.type, part->vertex_buffer.count,
						part->vertex_buffer.offset, part->vertex_buffer.base_address);
				}
			}
			if ((part->flags & 1) || !strip || !vertex_data || vertex_count <= 0 || part->triangle_buffer.type != 1 ||
				(part->vertex_buffer.type != 4 && part->vertex_buffer.type != 5 && part->vertex_buffer.base_address))
			{
				continue;
			}
			/* the part's shader: a transparent one (a shield's, glass, a
			hologram) is not in the rays; an alpha-tested one - a model's, unless
			it says not; an environment's that says so - is a cutout, by its
			base map's alpha, its coordinates scaled as the renderer scales them */
			{
				const byte *shader = NULL;
				short shader_type = 4;
				unsigned short shader_flags = 0;

				part_mask = -1;
				u_scale = model->base_map_scale.i != 0.0f ? model->base_map_scale.i : 1.0f;
				v_scale = model->base_map_scale.j != 0.0f ? model->base_map_scale.j : 1.0f;
				if (part->shader_index >= 0 && part->shader_index < model->shaders.count)
				{
					const struct tag_reference *reference = (const struct tag_reference *)
						((const byte *)model->shaders.address + part->shader_index * 32);

					if (reference->index != NONE)
						shader = (const byte *)tag_get(0x73686472 /* 'shdr' */, reference->index);
				}
				if (shader)
				{
					shader_type = *(const short *)(shader + 0x24);
					shader_flags = *(const unsigned short *)(shader + 0x28);
				}
				if (shader_type != 3 && shader_type != 4)
					continue;
				/* (its colour in the rays: its base map's average) */
				part_albedo = 0.0f;
				if (shader)
				{
					const struct tag_reference *base_map = (const struct tag_reference *)
						(shader + (shader_type == 4 ? 0xA4 : 0x88));

					if (base_map->index != NONE)
						part_albedo = object_albedo(bitmap_group_try_and_get_bitmap(base_map->index, 0));
				}
				if (shader && ((shader_type == 4 && !(shader_flags & 4)) || (shader_type == 3 && (shader_flags & 1))))
				{
					const struct tag_reference *base_map = (const struct tag_reference *)
						(shader + (shader_type == 4 ? 0xA4 : 0x88));

					if (base_map->index != NONE)
						part_mask = mask_index(bitmap_group_try_and_get_bitmap(base_map->index, 0));
					if (shader_type == 4)
					{
						float map_u = *(const float *)(shader + 0x9C), map_v = *(const float *)(shader + 0xA0);

						u_scale *= map_u != 0.0f ? map_u : 1.0f;
						v_scale *= map_v != 0.0f ? map_v : 1.0f;
					}
				}
			}
			/* (HALO_RT_LOG_SHAPES: each model's first parts' shaders, once) */
			if (getenv("HALO_RT_LOG_SHAPES") && part_index < 3 && part->shader_index >= 0 &&
				part->shader_index < model->shaders.count)
			{
				extern void platform_log(const char *format, ...);
				static long seen[256];
				static int seen_count;
				long key = definition->object.model.index * 64 + geometry_index * 4 + part_index;
				int k, found = 0;

				for (k = 0; k < seen_count; k++)
					found |= seen[k] == key;
				if (!found && seen_count < 256)
				{
					const struct tag_reference *reference = (const struct tag_reference *)
						((const byte *)model->shaders.address + part->shader_index * 32);
					const byte *part_shader = reference->index != NONE ? (const byte *)tag_get(0x73686472 /* 'shdr' */,
						reference->index) : NULL;

					seen[seen_count++] = key;
					if (part_shader)
						platform_log("ray tracing: %s part %d: shader %s, type %d, flags %04x",
							tag_get_name(definition->object.model.index), part_index, tag_get_name(reference->index),
							*(const short *)(part_shader + 0x24), *(const unsigned short *)(part_shader + 0x28));
				}
			}
			/* (HALO_RT_LOG_SHAPES: a part whose model-space vertices are far
			off, with its buffers, once each) */
			if (getenv("HALO_RT_LOG_SHAPES"))
			{
				extern void platform_log(const char *format, ...);
				static long logged[128];
				static int logged_count;
				long key = definition->object.model.index * 4096 + geometry_index * 64 + part_index, bad = 0, v;
				int seen = 0, k;

				for (v = 0; v < vertex_count; v++)
				{
					const float *position = (const float *)(vertex_data + v * vertex_size);

					if (!(position[0] * position[0] + position[1] * position[1] + position[2] * position[2] < 1e4f))
						bad++;
				}
				for (k = 0; k < logged_count; k++)
					seen |= logged[k] == key;
				if (bad && !seen && logged_count < 128)
				{
					const unsigned long *hardware = (const unsigned long *)part->vertex_buffer.hardware_format;

					logged[logged_count++] = key;
					platform_log("ray tracing: %s geometry %d part %d: %ld of %ld vertices far off; vertex buffer type %d "
						"count %ld offset %ld at %p, hardware %p (%08lx %08lx %08lx); strip %ld at %p; part flags %lx",
						tag_get_name(definition->object.model.index), geometry_index, part_index, bad, vertex_count,
						part->vertex_buffer.type, part->vertex_buffer.count, part->vertex_buffer.offset,
						part->vertex_buffer.base_address, hardware, hardware ? hardware[0] : 0, hardware ? hardware[1] : 0,
						hardware ? hardware[2] : 0, part->triangle_buffer.count, part->triangle_buffer.base_address,
						(unsigned long)part->flags);
				}
			}
			for (index = 0; index + 2 < strip_count && count < room; index++)
			{
				unsigned short corners[3] = { strip[index], strip[index + 1], strip[index + 2] };
				int corner;

				if (corners[0] == corners[1] || corners[1] == corners[2] || corners[0] == corners[2] ||
					corners[0] >= vertex_count || corners[1] >= vertex_count || corners[2] >= vertex_count)
				{
					continue;
				}
				for (corner = 0; corner < 3; corner++)
				{
					const byte *raw = vertex_data + corners[corner] * vertex_size;
					float *cut = triangle_cutout(out + count * 9);

					if (cut)
					{
						float u, v;

						if (compressed)
						{
							const struct model_vertex_view *view = (const struct model_vertex_view *)raw;

							u = (float)view->texture_coordinates[0] * (1.0f / 32767.0f);
							v = (float)view->texture_coordinates[1] * (1.0f / 32767.0f);
						}
						else
						{
							u = *(const float *)(raw + 48);
							v = *(const float *)(raw + 52);
						}
						cut[corner * 2 + 0] = u * u_scale;
						cut[corner * 2 + 1] = v * v_scale;
						cut[6] = (float)part_mask;
						cut[7] = part_albedo;
					}
					const real_point3d *position = (const real_point3d *)raw;
					short node0, node1;
					float weight0;
					real_point3d point0 = *position, point1 = *position;

					if (compressed)
					{
						const struct model_vertex_view *vertex = (const struct model_vertex_view *)raw;

						/* (three times the node's index, a byte: past 42 nodes, over 127) */
						node0 = (short)((unsigned char)vertex->node_indices[0] / 3);
						node1 = (short)((unsigned char)vertex->node_indices[1] / 3);
						weight0 = (float)vertex->node_weight * (1.0f / 32767.0f);
					}
					else
					{
						/* model_vertex_uncompressed: position, normal, binormal, tangent,
						texcoord (56 bytes), then two node indices and their weights */
						node0 = *(const short *)(raw + 56);
						node1 = *(const short *)(raw + 58);
						weight0 = *(const float *)(raw + 60);
					}
					float *q = out + count * 9 + corner * 3;

					if (node0 >= 0 && node0 < model->nodes.count)
						matrix4x3_transform_point(&relative[node0], position, &point0);
					if (node1 >= 0 && node1 < model->nodes.count)
						matrix4x3_transform_point(&relative[node1], position, &point1);
					else
						point1 = point0;
					q[0] = point0.x * weight0 + point1.x * (1.0f - weight0);
					q[1] = point0.y * weight0 + point1.y * (1.0f - weight0);
					q[2] = point0.z * weight0 + point1.z * (1.0f - weight0);
				}
				count++;
			}
		}
	}
	return count;
}

/* the object's triangles, if they are about where the object is (within
three times its bounding sphere, and more: what it carries too); none if any
is not - a model posed from nodes the game has not placed yet reaches
across the level, and wraps the camera */
static long object_triangles_sane(long object_index, const struct object_datum *object, const float *triangles,
	long count)
{
	float reach = object->object.bounding_sphere_radius * 3.0f + 3.0f;
	long index;

	for (index = 0; index < count * 3; index++)
	{
		float dx = triangles[index * 3 + 0] - object->object.bounding_sphere_center.x;
		float dy = triangles[index * 3 + 1] - object->object.bounding_sphere_center.y;
		float dz = triangles[index * 3 + 2] - object->object.bounding_sphere_center.z;

		if (!(dx * dx + dy * dy + dz * dz <= reach * reach))
		{
			static long logged[8];
			long slot;

			for (slot = 0; slot < 8 && logged[slot] != object->definition_index; slot++)
			{
				if (!logged[slot])
				{
					logged[slot] = object->definition_index;
					platform_log("ray tracing: %s's shape reaches %.1f from it (its radius %.1f): posed from nodes not placed",
						tag_get_name(object->definition_index), sqrtf(dx * dx + dy * dy + dz * dz),
						object->object.bounding_sphere_radius);
					break;
				}
			}
			(void)object_index;
			return 0;
		}
	}
	return count;
}

/* whether the last object_shapes made the drawn model's triangles (with
their cutouts) */
static boolean ray_model_made;

/* one object's triangles into out (at most room), of the shapes; returns
how many */
static long object_shapes(long object_index, struct object_datum *object, float *out, long room, long shapes)
{
	const struct object_definition *definition = object_definition_get(object->definition_index);
	const real_matrix4x3 *matrices = object_get_node_matrices(object_index);
	float radius = object->object.bounding_sphere_radius;
	long count = 0;

	/* the drawn model */
	if (shapes == _ray_shapes_model)
	{
		count = model_triangles(object, matrices, out, room, NULL);
		if (count > 0 && object_triangles_sane(object_index, object, out, count))
		{
			ray_model_made = TRUE;
			return count;
		}
		/* (posed from nodes the game has not placed - a turret's, at the
		world's origin: the model at rest where the object is, exact for
		what does not bend) */
		if (count > 0)
		{
			real_matrix4x3 placed;

			object_get_world_matrix(object_index, &placed);
			count = model_triangles(object, matrices, out, room, &placed);
			if (count > 0 && object_triangles_sane(object_index, object, out, count))
			{
				ray_model_made = TRUE;
				return count;
			}
		}
		count = 0;
	}
	/* the collision model: its meshes, where the game's bullets hit */
	if (matrices && definition->object.collision_model.index != NONE)
	{
		const struct collision_model *model = collision_model_definition_get(definition->object.collision_model.index);
		const struct collision_node *nodes = (const struct collision_node *)model->nodes.address;
		short node_index;

		for (node_index = 0; node_index < model->nodes.count && count < room; node_index++)
		{
			const struct collision_node *node = &nodes[node_index];
			const struct mesh_triangles *mesh;
			short permutation;
			long triangle;

			if (node->region_index == NONE || node->bsps.count <= 0)
				continue;
			permutation = object->object.region_permutations[node->region_index];
			if (permutation == NONE)
				continue;
			permutation = PIN(permutation, 0, node->bsps.count - 1);
			mesh = mesh_get((const struct collision_bsp *)node->bsps.address + permutation);
			if (!mesh)
				continue;
			for (triangle = 0; triangle < mesh->count && count < room; triangle++, count++)
			{
				int corner;

				for (corner = 0; corner < 3; corner++)
				{
					const float *p = &mesh->triangles[triangle * 9 + corner * 3];
					real_point3d local = { p[0], p[1], p[2] }, placed;

					matrix4x3_transform_point(&matrices[node_index], &local, &placed);
					out[count * 9 + corner * 3 + 0] = placed.x;
					out[count * 9 + corner * 3 + 1] = placed.y;
					out[count * 9 + corner * 3 + 2] = placed.z;
				}
			}
		}
		if (count > 0)
			return count;
	}
	/* (without either: none - left out of the rays, not guessed at) */
	return 0;
}

/* one object's triangles, and their cutouts (none but a drawn model's) */
static long object_triangles(long object_index, struct object_datum *object, float *out, long room, long shapes)
{
	long count, index;

	ray_model_made = FALSE;
	count = object_shapes(object_index, object, out, room, shapes);
	if (!ray_model_made)
	{
		for (index = 0; index < count; index++)
		{
			float *cut = triangle_cutout(out + index * 9);

			if (cut)
			{
				cut[6] = -1.0f;
				cut[7] = 0.0f;
			}
		}
	}
	return count;
}

/* the objects in the rays: all that the game draws as models - units,
items, projectiles, scenery, devices */
#define RAY_TRACED_OBJECT_TYPES (_object_mask_unit | _object_mask_item | _object_mask_projectile | \
	_object_mask_scenery | _object_mask_device)

/* an object and what it carries (its children: a unit's weapon), in one
group; returns how many triangles */
static long object_family_triangles(long object_index, struct object_datum *object, float *out, long room,
	long shapes, unsigned char group, unsigned char *groups)
{
	long count = object_triangles(object_index, object, out, room, shapes), child_index, index, guard = 0;

	for (child_index = object->object.first_child_object_index; child_index != NONE && count < room && guard < 16;
		guard++)
	{
		struct object_datum *child = object_try_and_get_and_verify_type(child_index, RAY_TRACED_OBJECT_TYPES);

		if (!child)
			break;
		count += object_triangles(child_index, child, out + count * 9, room - count, shapes);
		child_index = child->object.next_object_index;
	}
	for (index = 0; index < count; index++)
		groups[index] = group;
	return count;
}

/* this frame's objects near the camera as triangles (9 floats each, at most
maximum) and each triangle's group, and the player's body's bounding sphere
(center, radius; radius 0 if none); returns how many triangles. The player
and what it carries are group 0; the other units (with what they carry)
each a group; the loose objects (items on the ground, projectiles,
scenery, devices) share the last. */
long halo_ray_tracing_objects(float *triangles, unsigned char *groups, float *cutouts, long maximum,
	const float *camera, float *player_sphere, long shapes)
{
	struct object_iterator iterator;
	struct object_datum *object;
	long count = 0, player_unit = NONE, player_index, group = 1, index;
	const unsigned char loose = (unsigned char)((RAY_TRACED_OBJECT_GROUPS - 1) << 3 | _ray_mask_object);

	player_sphere[0] = player_sphere[1] = player_sphere[2] = player_sphere[3] = 0.0f;
	ray_triangles_base = triangles;
	ray_cutouts = cutouts;
	if (global_structure_bsp_index == NONE || !object_header_data)
		return 0;
	player_index = local_player_get_player_index(0);
	if (player_index != NONE)
		player_unit = player_get(player_index)->unit_index;
	if (player_unit != NONE && (object = object_try_and_get_and_verify_type(player_unit, _object_mask_unit)) != NULL &&
		object->object.bounding_sphere_radius > 0.0f)
	{
		player_sphere[0] = object->object.bounding_sphere_center.x;
		player_sphere[1] = object->object.bounding_sphere_center.y;
		player_sphere[2] = object->object.bounding_sphere_center.z;
		player_sphere[3] = object->object.bounding_sphere_radius;
		count += object_triangles_sane(player_unit, object, triangles,
			object_family_triangles(player_unit, object, triangles, maximum, shapes, _ray_mask_player, groups));
	}
	/* (the objects near a point that moves only as the camera goes 4 units
	from it - 4 farther, all round: the same objects frame to frame, whose
	shapes the host keeps rather than builds again) */
	{
		static float anchor[3] = { 1e30f, 1e30f, 1e30f };
		float ax = camera[0] - anchor[0], ay = camera[1] - anchor[1], az = camera[2] - anchor[2];

		if (ax * ax + ay * ay + az * az > RAY_TRACED_OBJECT_ANCHOR_STEP * RAY_TRACED_OBJECT_ANCHOR_STEP)
		{
			anchor[0] = camera[0];
			anchor[1] = camera[1];
			anchor[2] = camera[2];
		}
		ray_object_anchor = anchor;
	}
	object_iterator_new(&iterator, RAY_TRACED_OBJECT_TYPES, 0);
	while ((object = (struct object_datum *)object_iterator_next(&iterator)) != NULL && count < maximum)
	{
		float radius = object->object.bounding_sphere_radius;
		float dx = object->object.bounding_sphere_center.x - ray_object_anchor[0];
		float dy = object->object.bounding_sphere_center.y - ray_object_anchor[1];
		float dz = object->object.bounding_sphere_center.z - ray_object_anchor[2];
		float reach = RAY_TRACED_OBJECT_DISTANCE + RAY_TRACED_OBJECT_ANCHOR_STEP + radius;
		boolean unit = ((1UL << object->object.type) & _object_mask_unit) != 0;
		long added;

		/* (what something carries goes with it) */
		if (iterator.index == player_unit || object->object.parent_object_index != NONE || !(radius > 0.0f) ||
			radius > 20.0f || dx * dx + dy * dy + dz * dz > reach * reach)
		{
			continue;
		}
		if (unit && group < RAY_TRACED_OBJECT_GROUPS - 1)
		{
			added = object_family_triangles(iterator.index, object, triangles + count * 9, maximum - count, shapes,
				(unsigned char)(group << 3 | _ray_mask_object), groups + count);
			added = object_triangles_sane(iterator.index, object, triangles + count * 9, added);
			if (added > 0)
				group++;
		}
		else
		{
			added = object_family_triangles(iterator.index, object, triangles + count * 9, maximum - count, shapes,
				loose, groups + count);
			added = object_triangles_sane(iterator.index, object, triangles + count * 9, added);
		}
		count += added;
	}
	(void)index;
	return count;
}

/* ---------- the emitters

The glowing things the game draws without a light of their own - a
needle, a plasma bolt's glow, Guilty Spark's eye, a glowing panel: any
object with a light volume (the glow's sprite) attached or as a widget, and
no light - as lights for the rays: each lights what is near it in its
glow's colour, with its shadows. (Those with a light the game draws it,
and the rays shadow it: object_lights.c.) */

#include "objects/widgets/light_volumes.h"

#define RAY_TRACED_EMITTER_DISTANCE 30.0f
/* how far an emitter's light reaches (world units), and how bright */
#define RAY_TRACED_EMITTER_RADIUS 3.0f
#define RAY_TRACED_EMITTER_INTENSITY 1.0f

#define GROUP_TAG_LIGHT_VOLUME 0x6D677332 /* 'mgs2' */
#define GROUP_TAG_LIGHT 0x6C696768 /* 'ligh' */

/* a light volume's colour (its first frame's near colour); FALSE if dark */
static boolean light_volume_color(long definition_index, float *color)
{
	const struct light_volume_definition *volume = light_volume_definition_get(definition_index);
	const struct light_volume_frame *frame;

	if (volume->frames.count <= 0)
		return FALSE;
	frame = (const struct light_volume_frame *)volume->frames.address;
	color[0] = frame->color_hither.red;
	color[1] = frame->color_hither.green;
	color[2] = frame->color_hither.blue;
	return color[0] + color[1] + color[2] > 0.05f;
}

/* the colour of the glow the definition attaches (or has as a widget), if
it attaches no light; FALSE if none */
static boolean emitter_color(const struct object_definition *definition, float *color)
{
	const struct object_attachment_definition *attachments =
		(const struct object_attachment_definition *)definition->object.attachments.address;
	const struct object_definition_widget *widgets =
		(const struct object_definition_widget *)definition->object.widgets.address;
	long index;
	boolean found = FALSE;

	for (index = 0; index < definition->object.widgets.count && !found; index++)
	{
		if (widgets[index].type.group_tag == GROUP_TAG_LIGHT_VOLUME && widgets[index].type.index != NONE)
			found = light_volume_color(widgets[index].type.index, color);
	}
	for (index = 0; index < definition->object.attachments.count; index++)
	{
		const struct tag_reference *type = &attachments[index].type;

		if (type->group_tag == GROUP_TAG_LIGHT && type->index != NONE)
			return FALSE;
		if (!found && type->group_tag == GROUP_TAG_LIGHT_VOLUME && type->index != NONE)
			found = light_volume_color(type->index, color);
	}
	return found;
}

/* this frame's emitters near the camera, 8 floats each (the position, the
radius, the colour, the intensity); returns how many, at most maximum */
long halo_ray_tracing_emitters(float *emitters, long maximum, const float *camera)
{
	struct object_iterator iterator;
	struct object_datum *object;
	long count = 0, player_unit = NONE, player_index;

	if (global_structure_bsp_index == NONE || !object_header_data)
		return 0;
	player_index = local_player_get_player_index(0);
	if (player_index != NONE)
		player_unit = player_get(player_index)->unit_index;
	object_iterator_new(&iterator, RAY_TRACED_OBJECT_TYPES, 0);
	while ((object = (struct object_datum *)object_iterator_next(&iterator)) != NULL && count < maximum)
	{
		/* (a projectile at its point; anything larger at its middle) */
		const real_point3d *at = object->object.type == _object_type_projectile ? &object->object.position :
			&object->object.bounding_sphere_center;
		float dx = at->x - camera[0], dy = at->y - camera[1], dz = at->z - camera[2], color[3];
		float *out = emitters + count * 8;

		/* (not what you carry: your weapon's glow is at the camera, and the
		first person draws it on the gun) */
		if (dx * dx + dy * dy + dz * dz > RAY_TRACED_EMITTER_DISTANCE * RAY_TRACED_EMITTER_DISTANCE ||
			(player_unit != NONE && (iterator.index == player_unit ||
				(object->object.parent_object_index != NONE &&
					object_get_ultimate_parent(iterator.index) == player_unit))) ||
			!emitter_color(object_definition_get(object->definition_index), color))
		{
			continue;
		}
		out[0] = at->x;
		out[1] = at->y;
		out[2] = at->z;
		/* (a large object's glow reaches a little farther, not across the level) */
		out[3] = PIN(object->object.bounding_sphere_radius * 2.0f, RAY_TRACED_EMITTER_RADIUS, 12.0f);
		out[4] = color[0];
		out[5] = color[1];
		out[6] = color[2];
		out[7] = RAY_TRACED_EMITTER_INTENSITY;
		count++;
	}
	return count;
}

/* ---------- the drawn level

The level as the game draws it, for the traced lighting's rays
(display.ray_tracing_level "render"): each lightmap's materials' triangles
(structure_bsp.lightmaps, .surfaces), in the world, with their lightmap
coordinates, and each material's surface as the rays need it - its colour
(its base map's average, once the texture cache has it), the light it gives
off (its shader's radiosity: the level's lamps and glowing panels, from
which the lightmaps were baked), and its lightmap page. The pages (the
lightmaps' bitmaps, decoded as the texture cache loads them) are the
light already on every surface: where a ray lands, the light it finds.

Only the environment shaders' materials shade the rays; the transparent
ones (water, glass) are flagged, and the rays pass through them. */

#include "structures/structure_bsp_definitions.h"
#include "shaders/shader_definitions.h"
#include "bitmaps/bitmap_group.h"
#include "bitmaps/bitmap_group_lookup.h"
#include "bitmaps/bitmaps_sampling.h"
#include "cache/texture_cache.h"
#include "rasterizer/rasterizer_geometry_environment.h"

enum
{
	_ray_level_shader_type_environment = 3,
	_ray_level_material_transparent = 1,
	/* the environment vertex (position, packed normal, binormal, tangent,
	texcoord) and the lightmap vertex (packed incident direction, u, v) as
	the cache files keep them, the lightmap vertices after the material's
	vertices */
	_ray_level_vertex_size = 32,
	_ray_level_lightmap_vertex_size = 8,
	RAY_LEVEL_MATERIAL_FLOATS = 8,
};

/* a shader_environment's base map (object_lights.c's view of it) */
struct ray_level_shader_environment
{
	struct shader shader;
	byte reserved28[0x60];
	struct tag_reference base_map;
	byte reserved98[0xE8];
	/* the self-illumination (rasterizer_xbox_environment.c): its map's red,
	green and blue say where the primary, secondary and plasma colours glow;
	without a map, nothing glows */
	word self_illumination_flags;
	short pad182;
	byte reserved184[0x18];
	real_rgb_color primary_on_color;
	byte reserved1A8[0x30];
	real_rgb_color secondary_on_color;
	byte reserved1E4[0x30];
	real_rgb_color plasma_on_color;
	byte reserved220[0x30];
	real self_illumination_map_scale;
	struct tag_reference self_illumination_map;
};
typedef char ray_level_shader_primary_offset_assert[
	offsetof(struct ray_level_shader_environment, primary_on_color) == 0x19C ? 1 : -1];
typedef char ray_level_shader_self_illumination_map_offset_assert[
	offsetof(struct ray_level_shader_environment, self_illumination_map) == 0x254 ? 1 : -1];

/* the light a shader's radiosity power gives off, as the rays take it: its
power is all it gives off, each way together, and the rays take the light
one way - pi times it. (Against the lightmaps, the exposure then: Chill Out,
lit by its panels alone, about 1.3; c10's halls about 3; b30 outdoors, lit
by the sun, 1.1) */
#define RAY_RADIOSITY_POWER_SCALE 3.14159265f

/* how bright a self-illuminated surface's light is, of its colour (a panel
drawn at full brightness lights what faces it, near, about as much) */
#define RAY_SELF_ILLUMINATION_POWER 4.0f

static struct
{
	const struct structure_bsp *bsp;
	unsigned long generation;
	float *vertices;
	float *texcoords;
	/* each vertex's base map coordinates (for the cutouts) */
	float *base_texcoords;
	unsigned long *indices;
	unsigned long *triangle_materials;
	long vertex_count, triangle_count;
	/* the alpha-tested triangles are last, from this one */
	long cutout_start;
	/* RAY_LEVEL_MATERIAL_FLOATS each: the colour, the flags (1 transparent;
	16 times one more than its cutout's mask, if it is alpha-tested); the
	light given off, the lightmap page (-1 none) */
	float *materials;
	long material_count;
	/* each material's base map, until its colour is known */
	struct bitmap_data **base_maps;
	/* each material's self-illumination map, until its glow is known, and
	its primary, secondary and plasma colours (9 floats) */
	struct bitmap_data **self_maps;
	float *self_colors;
	boolean materials_changed;
	/* each page's bitmap (the lightmap's), and whether it is decoded */
	struct bitmap_data **pages;
	boolean *pages_done;
	long page_count;
	unsigned char *page_pixels;
	long page_pixels_size;
	long next_material;
} level;

/* whether bitmap_2d_get_pixel can read the bitmap (a 2D one, not linear, in
a format it decodes: bitmaps.c), and the texture cache has it (asked for,
without waiting) */
static boolean level_bitmap_readable(struct bitmap_data *bitmap)
{
	/* a8 y8 ay8 a8y8, r5g6b5, a1r5g5b5 a4r4g4b4 x8r8g8b8 a8r8g8b8, dxt1 dxt3 dxt5 */
	static const unsigned long formats = 0xF | (1 << 6) | (0xF << 8) | (0x7 << 14);

	return bitmap->type == 0 && !(bitmap->flags & (1 << 4)) && bitmap->format >= 0 && bitmap->format < 32 &&
		(formats & (1UL << bitmap->format)) && _texture_cache_bitmap_get_hardware_format(bitmap, FALSE, TRUE) &&
		bitmap->base_address;
}

/* the cutouts' masks: the alpha of the textures of what is alpha-tested
(the level's foliage and fences, the plants' and the objects' models), as
the rays need it to see through their holes - each a mipmap of at most
128x128, decoded once the texture cache has it */
#define RAY_MASKS 256

static struct
{
	struct bitmap_data *bitmaps[RAY_MASKS];
	boolean done[RAY_MASKS];
	long count;
	unsigned long generation;
	unsigned char pixels[128 * 128];
} masks;

/* the mask of a bitmap's alpha, asked for; -1 if there is no room */
static long mask_index(struct bitmap_data *bitmap)
{
	long index;

	if (!bitmap)
		return -1;
	for (index = 0; index < masks.count; index++)
	{
		if (masks.bitmaps[index] == bitmap)
			return index;
	}
	if (masks.count >= RAY_MASKS)
		return -1;
	masks.bitmaps[masks.count] = bitmap;
	masks.done[masks.count] = FALSE;
	return masks.count++;
}

static boolean level_bitmap_readable(struct bitmap_data *bitmap);

/* the next mask the texture cache has the bitmap of, decoded (its alpha,
a byte a texel); FALSE when none is ready. *generation changes when the
masks start anew (a new level) */
boolean halo_ray_tracing_mask(long *index, const unsigned char **alpha, long *width, long *height,
	unsigned long *generation)
{
	long mask;

	*generation = masks.generation;
	for (mask = 0; mask < masks.count; mask++)
	{
		struct bitmap_data *bitmap = masks.bitmaps[mask];
		long mipmap = 0, x, y, w, h;
		float lod = 1.0f;

		if (masks.done[mask] || !level_bitmap_readable(bitmap))
			continue;
		while (mipmap < bitmap->mipmap_count && ((bitmap->width >> mipmap) > 128 || (bitmap->height >> mipmap) > 128) &&
			(bitmap->width >> (mipmap + 1)) >= 8 && (bitmap->height >> (mipmap + 1)) >= 8)
		{
			mipmap++;
		}
		if (bitmap->mipmap_count > 0)
			lod = 1.0f - ((float)mipmap + 0.25f) / (float)bitmap->mipmap_count;
		w = MAX(bitmap->width >> mipmap, 1);
		h = MAX(bitmap->height >> mipmap, 1);
		if (w > 128 || h > 128)
		{
			masks.done[mask] = TRUE;
			continue;
		}
		for (y = 0; y < h; y++)
		{
			for (x = 0; x < w; x++)
			{
				real_point2d point;

				point.x = ((float)x + 0.5f) / (float)w;
				point.y = ((float)y + 0.5f) / (float)h;
				masks.pixels[y * w + x] = (unsigned char)(bitmap_2d_get_pixel(bitmap, &point, lod) >> 24);
			}
		}
		masks.done[mask] = TRUE;
		*index = mask;
		*alpha = masks.pixels;
		*width = w;
		*height = h;
		return TRUE;
	}
	return FALSE;
}

static void level_free(void)
{
	void **blocks[] = { (void **)&level.vertices, (void **)&level.texcoords, (void **)&level.base_texcoords,
		(void **)&level.indices,
		(void **)&level.triangle_materials, (void **)&level.materials, (void **)&level.base_maps,
		(void **)&level.self_maps, (void **)&level.self_colors,
		(void **)&level.pages, (void **)&level.pages_done };
	long index;

	for (index = 0; index < (long)(sizeof(blocks) / sizeof(blocks[0])); index++)
	{
		if (*blocks[index])
			free(*blocks[index]);
		*blocks[index] = NULL;
	}
	level.vertex_count = level.triangle_count = level.material_count = level.page_count = 0;
	level.next_material = 0;
}

static void level_build(const struct structure_bsp *bsp)
{
	long lightmap_index, material_count = 0, vertex_count = 0, triangle_count = 0;

	level_free();
	level.bsp = bsp;
	level.generation++;
	masks.count = 0;
	masks.generation++;
	if (!bsp)
		return;
	/* the sizes */
	for (lightmap_index = 0; lightmap_index < bsp->lightmaps.count; lightmap_index++)
	{
		const struct structure_lightmap *lightmap = TAG_BLOCK_GET_ELEMENT(&bsp->lightmaps, lightmap_index,
			struct structure_lightmap);
		long material_index;

		for (material_index = 0; material_index < lightmap->materials.count; material_index++)
		{
			const struct structure_material *material = TAG_BLOCK_GET_ELEMENT(&lightmap->materials,
				material_index, struct structure_material);

			material_count++;
			if (material->compressed_vertex_data.address && material->vertices.count > 0)
			{
				vertex_count += material->vertices.count;
				triangle_count += material->surface_count;
			}
		}
	}
	if (!material_count || !vertex_count || !triangle_count)
		return;
	level.vertices = malloc((size_t)vertex_count * 3 * sizeof(float));
	level.texcoords = malloc((size_t)vertex_count * 2 * sizeof(float));
	level.base_texcoords = malloc((size_t)vertex_count * 2 * sizeof(float));
	level.indices = malloc((size_t)triangle_count * 3 * sizeof(unsigned long));
	level.triangle_materials = malloc((size_t)triangle_count * sizeof(unsigned long));
	level.materials = malloc((size_t)material_count * RAY_LEVEL_MATERIAL_FLOATS * sizeof(float));
	level.base_maps = malloc((size_t)material_count * sizeof(struct bitmap_data *));
	level.self_maps = malloc((size_t)material_count * sizeof(struct bitmap_data *));
	level.self_colors = malloc((size_t)material_count * 9 * sizeof(float));
	level.pages = malloc((size_t)MAX(bsp->lightmaps.count, 1) * sizeof(struct bitmap_data *));
	level.pages_done = malloc((size_t)MAX(bsp->lightmaps.count, 1) * sizeof(boolean));
	if (!level.vertices || !level.texcoords || !level.base_texcoords || !level.indices || !level.triangle_materials || !level.materials ||
		!level.base_maps || !level.self_maps || !level.self_colors || !level.pages || !level.pages_done)
	{
		level_free();
		return;
	}
	level.page_count = bsp->lightmaps.count;
	for (lightmap_index = 0; lightmap_index < bsp->lightmaps.count; lightmap_index++)
	{
		const struct structure_lightmap *lightmap = TAG_BLOCK_GET_ELEMENT(&bsp->lightmaps, lightmap_index,
			struct structure_lightmap);
		long material_index;

		level.pages[lightmap_index] = bsp->lightmap_group.index != NONE && lightmap->bitmap_index != NONE ?
			bitmap_group_try_and_get_bitmap(bsp->lightmap_group.index, lightmap->bitmap_index) : NULL;
		level.pages_done[lightmap_index] = FALSE;
		for (material_index = 0; material_index < lightmap->materials.count; material_index++)
		{
			const struct structure_material *material = TAG_BLOCK_GET_ELEMENT(&lightmap->materials,
				material_index, struct structure_material);
			const struct shader *shader = material->shader.index != NONE ?
				shader_definition_get(material->shader.index) : NULL;
			float *out = level.materials + level.material_count * RAY_LEVEL_MATERIAL_FLOATS;
			const byte *vertices = (const byte *)material->compressed_vertex_data.address;
			long first_vertex = level.vertex_count, vertex_index, surface_offset;
			boolean opaque = shader && shader->base.type == _ray_level_shader_type_environment;

			/* (a grey until the base map is read) */
			out[0] = out[1] = out[2] = 0.5f;
			out[3] = opaque ? 0.0f : (float)_ray_level_material_transparent;
			out[4] = out[5] = out[6] = 0.0f;
			if (shader && shader->base.radiosity.power > 0.0f)
			{
				out[4] = shader->base.radiosity.color_of_emitted_light.red * shader->base.radiosity.power *
					RAY_RADIOSITY_POWER_SCALE;
				out[5] = shader->base.radiosity.color_of_emitted_light.green * shader->base.radiosity.power *
					RAY_RADIOSITY_POWER_SCALE;
				out[6] = shader->base.radiosity.color_of_emitted_light.blue * shader->base.radiosity.power *
					RAY_RADIOSITY_POWER_SCALE;
			}
			out[7] = level.pages[lightmap_index] ? (float)lightmap_index : -1.0f;
			level.base_maps[level.material_count] = NULL;
			level.self_maps[level.material_count] = NULL;
			if (opaque)
			{
				const struct ray_level_shader_environment *environment =
					(const struct ray_level_shader_environment *)shader;

				/* (self-illuminated: its glow once its map is read) */
				if (environment->self_illumination_map.index != NONE)
				{
					const struct bitmap_group *group = bitmap_group_get(environment->self_illumination_map.index);
					float *colors = level.self_colors + level.material_count * 9;

					colors[0] = environment->primary_on_color.red;
					colors[1] = environment->primary_on_color.green;
					colors[2] = environment->primary_on_color.blue;
					colors[3] = environment->secondary_on_color.red;
					colors[4] = environment->secondary_on_color.green;
					colors[5] = environment->secondary_on_color.blue;
					colors[6] = environment->plasma_on_color.red;
					colors[7] = environment->plasma_on_color.green;
					colors[8] = environment->plasma_on_color.blue;
					if (group && group->bitmaps.count > 0 &&
						colors[0] + colors[1] + colors[2] + colors[3] + colors[4] + colors[5] + colors[6] + colors[7] +
						colors[8] > 0.0f)
					{
						level.self_maps[level.material_count] = bitmap_group_try_and_get_bitmap(
							environment->self_illumination_map.index,
							(short)(material->permutation_index % group->bitmaps.count));
					}
				}

				if (environment->base_map.index != NONE)
				{
					const struct bitmap_group *group = bitmap_group_get(environment->base_map.index);

					if (group && group->bitmaps.count > 0)
					{
						level.base_maps[level.material_count] = bitmap_group_try_and_get_bitmap(
							environment->base_map.index,
							(short)(material->permutation_index % group->bitmaps.count));
						/* (alpha-tested: a cutout, by its base map's alpha) */
						if (*(const unsigned short *)((const byte *)shader + 0x28) & 1)
						{
							long mask = mask_index(level.base_maps[level.material_count]);

							if (mask >= 0)
								out[3] += (float)((mask + 1) * 16);
						}
					}
				}
			}
			if (!vertices || material->vertices.count <= 0)
			{
				level.material_count++;
				continue;
			}
			for (vertex_index = 0; vertex_index < material->vertices.count; vertex_index++)
			{
				real_point3d point;
				real_point2d texcoord;

				environment_vertex_compressed_get_point(
					(const struct environment_vertex_compressed *)(vertices + vertex_index * _ray_level_vertex_size),
					&point);
				level.vertices[level.vertex_count * 3 + 0] = point.x;
				level.vertices[level.vertex_count * 3 + 1] = point.y;
				level.vertices[level.vertex_count * 3 + 2] = point.z;
				texcoord.x = texcoord.y = 0.0f;
				if (level.pages[lightmap_index])
				{
					environment_lightmap_vertex_compressed_get_texcoord(
						(const struct environment_lightmap_vertex_compressed *)(vertices +
							material->vertices.count * _ray_level_vertex_size +
							vertex_index * _ray_level_lightmap_vertex_size),
						&texcoord);
				}
				level.texcoords[level.vertex_count * 2 + 0] = texcoord.x;
				level.texcoords[level.vertex_count * 2 + 1] = texcoord.y;
				environment_vertex_compressed_get_texcoord(
					(const struct environment_vertex_compressed *)(vertices + vertex_index * _ray_level_vertex_size),
					&texcoord);
				level.base_texcoords[level.vertex_count * 2 + 0] = texcoord.x;
				level.base_texcoords[level.vertex_count * 2 + 1] = texcoord.y;
				level.vertex_count++;
			}
			for (surface_offset = 0; surface_offset < material->surface_count; surface_offset++)
			{
				long surface_index = material->first_surface_index + surface_offset;
				const struct structure_surface *surface;
				long a, b, c;

				if (surface_index < 0 || surface_index >= bsp->surfaces.count)
					break;
				surface = TAG_BLOCK_GET_ELEMENT(&bsp->surfaces, surface_index, struct structure_surface);
				a = surface->vertex_indices[0];
				b = surface->vertex_indices[1];
				c = surface->vertex_indices[2];
				if (a >= material->vertices.count || b >= material->vertices.count || c >= material->vertices.count)
					continue;
				/* wound counterclockwise around the side the vertices' normals
				face, as the collision surfaces are (the rays see that side as
				the front) */
				{
					const float *pa = &level.vertices[(first_vertex + a) * 3];
					const float *pb = &level.vertices[(first_vertex + b) * 3];
					const float *pc = &level.vertices[(first_vertex + c) * 3];
					float u[3] = { pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2] };
					float v[3] = { pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2] };
					real_vector3d normal;

					environment_vertex_compressed_get_normal(
						(const struct environment_vertex_compressed *)(vertices + a * _ray_level_vertex_size), &normal);
					if ((u[1] * v[2] - u[2] * v[1]) * normal.i + (u[2] * v[0] - u[0] * v[2]) * normal.j +
						(u[0] * v[1] - u[1] * v[0]) * normal.k < 0.0f)
					{
						long swap = b;

						b = c;
						c = swap;
					}
				}
				level.indices[level.triangle_count * 3 + 0] = (unsigned long)(first_vertex + a);
				level.indices[level.triangle_count * 3 + 1] = (unsigned long)(first_vertex + b);
				level.indices[level.triangle_count * 3 + 2] = (unsigned long)(first_vertex + c);
				level.triangle_materials[level.triangle_count] = (unsigned long)level.material_count;
				level.triangle_count++;
			}
			level.material_count++;
		}
	}
	/* the alpha-tested triangles last (the rays test them apart); the
	transparent ones (water, glass) left out - the rays pass through them
	(in the rays, water's surface shadowed the shore and the shallows
	black) */
	{
		long read, write = 0, cut = 0, transparent = 0;
		unsigned long *indices = malloc((size_t)MAX(level.triangle_count, 1) * 3 * sizeof(unsigned long));
		unsigned long *triangle_materials = malloc((size_t)MAX(level.triangle_count, 1) * sizeof(unsigned long));

		if (indices && triangle_materials)
		{
			long pass;

			for (pass = 0; pass < 2; pass++)
			{
				for (read = 0; read < level.triangle_count; read++)
				{
					unsigned long material = level.triangle_materials[read];
					float flags = level.materials[material * RAY_LEVEL_MATERIAL_FLOATS + 3];
					boolean cutout = flags >= 16.0f;

					if (flags == (float)_ray_level_material_transparent)
					{
						transparent += pass == 0;
						continue;
					}
					if (cutout != (pass == 1))
						continue;
					memcpy(indices + write * 3, level.indices + read * 3, 3 * sizeof(unsigned long));
					triangle_materials[write] = material;
					write++;
					cut += pass;
				}
				if (pass == 0)
					level.cutout_start = write;
			}
			level.triangle_count = write;
			memcpy(level.indices, indices, (size_t)level.triangle_count * 3 * sizeof(unsigned long));
			memcpy(level.triangle_materials, triangle_materials, (size_t)level.triangle_count * sizeof(unsigned long));
			platform_log("ray tracing: %ld of the level's triangles are cutouts (alpha-tested), %ld transparent (left out)",
				cut, transparent);
		}
		else
		{
			level.cutout_start = level.triangle_count;
		}
		if (indices)
			free(indices);
		if (triangle_materials)
			free(triangle_materials);
	}
	level.materials_changed = TRUE;
	if (getenv("HALO_RT_LOG_SHAPES"))
	{
		long cutouts = 0, logged = 0;

		for (lightmap_index = 0; lightmap_index < bsp->lightmaps.count; lightmap_index++)
		{
			const struct structure_lightmap *lightmap = TAG_BLOCK_GET_ELEMENT(&bsp->lightmaps, lightmap_index,
				struct structure_lightmap);
			long material_index;

			for (material_index = 0; material_index < lightmap->materials.count; material_index++)
			{
				const struct structure_material *material = TAG_BLOCK_GET_ELEMENT(&lightmap->materials,
					material_index, struct structure_material);
				const struct shader *shader = material->shader.index != NONE ?
					shader_definition_get(material->shader.index) : NULL;

				if (shader && shader->base.type == _ray_level_shader_type_environment &&
					(*(const unsigned short *)((const byte *)shader + 0x28) & 1))
				{
					cutouts++;
					if (logged++ < 8)
						platform_log("ray tracing: alpha-tested level material %s, %ld triangles",
							tag_get_name(material->shader.index), (long)material->surface_count);
				}
				else if (shader && shader->base.type != _ray_level_shader_type_environment && logged < 16)
				{
					logged++;
					platform_log("ray tracing: level material %s, shader type %d", tag_get_name(material->shader.index),
						shader->base.type);
				}
			}
		}
		platform_log("ray tracing: %ld alpha-tested level materials", cutouts);
	}
	platform_log("ray tracing: the level's bounds %.1f..%.1f %.1f..%.1f %.1f..%.1f", bsp->world_bounds.x0,
		bsp->world_bounds.x1, bsp->world_bounds.y0, bsp->world_bounds.y1, bsp->world_bounds.z0, bsp->world_bounds.z1);
	/* (the brightest glowing materials, where they are: for test cameras) */
	{
		long logged = 0, index;
		float threshold = 1e30f;

		while (logged < (getenv("HALO_RT_LOG_SHAPES") ? 40 : 6))
		{
			float best = 0.0f;
			long best_index = NONE, lightmap_best = 0, material_best = 0;

			index = 0;
			for (lightmap_index = 0; lightmap_index < bsp->lightmaps.count; lightmap_index++)
			{
				const struct structure_lightmap *lightmap = TAG_BLOCK_GET_ELEMENT(&bsp->lightmaps, lightmap_index,
					struct structure_lightmap);
				long material_index;

				for (material_index = 0; material_index < lightmap->materials.count; material_index++, index++)
				{
					const float *m = level.materials + index * RAY_LEVEL_MATERIAL_FLOATS;
					float power = m[4] + m[5] + m[6];

					if (power > best && power < threshold)
					{
						best = power;
						best_index = index;
						lightmap_best = lightmap_index;
						material_best = material_index;
					}
				}
			}
			if (best_index == NONE)
				break;
			{
				const struct structure_lightmap *lightmap = TAG_BLOCK_GET_ELEMENT(&bsp->lightmaps, lightmap_best,
					struct structure_lightmap);
				const struct structure_material *material = TAG_BLOCK_GET_ELEMENT(&lightmap->materials, material_best,
					struct structure_material);

				const byte *vertices = (const byte *)material->compressed_vertex_data.address;
				float center[3] = { 0.0f, 0.0f, 0.0f };
				long vertex_index;

				for (vertex_index = 0; vertices && vertex_index < material->vertices.count; vertex_index++)
				{
					const float *point = (const float *)(vertices + vertex_index * _ray_level_vertex_size);

					center[0] += point[0] / (float)material->vertices.count;
					center[1] += point[1] / (float)material->vertices.count;
					center[2] += point[2] / (float)material->vertices.count;
				}
				{
					const float *glow = level.materials + best_index * RAY_LEVEL_MATERIAL_FLOATS + 4;

					platform_log("ray tracing: glowing material %s: %.2f (%.2f %.2f %.2f), %ld vertices about %.1f %.1f %.1f",
						material->shader.index != NONE ? tag_get_name(material->shader.index) : "?", best, glow[0], glow[1],
						glow[2], (long)material->vertices.count, center[0], center[1], center[2]);
				}
				/* (and its first vertex, and the way it faces - its packed normal,
				11, 11 and 10 bits: for test cameras in front of it) */
				if (vertices && material->vertices.count > 0)
				{
					const float *point = (const float *)vertices;
					unsigned long packed = *(const unsigned long *)(vertices + 12);
					long x = (long)(packed & 0x7FF), y = (long)((packed >> 11) & 0x7FF), z = (long)(packed >> 22);

					x = x >= 1024 ? x - 2048 : x;
					y = y >= 1024 ? y - 2048 : y;
					z = z >= 512 ? z - 1024 : z;
					platform_log("ray tracing:   a vertex %.2f %.2f %.2f facing %.2f %.2f %.2f", point[0], point[1], point[2],
						(float)x / 1023.0f, (float)y / 1023.0f, (float)z / 511.0f);
				}
			}
			threshold = best;
			logged++;
		}
	}
}

/* the active BSP's drawn triangles: returns its generation, which changes
when the BSP does; 0 while there is none */
unsigned long halo_ray_tracing_level(const float **vertices, const float **texcoords, const float **base_texcoords,
	long *vertex_count, const unsigned long **indices, const unsigned long **triangle_materials, long *triangle_count,
	long *cutout_start)
{
	const struct structure_bsp *bsp = global_structure_bsp_index != NONE ? global_structure_bsp_get() : NULL;

	if (bsp != level.bsp)
		level_build(bsp);
	if (!bsp || !level.triangle_count)
		return 0;
	*vertices = level.vertices;
	*texcoords = level.texcoords;
	*base_texcoords = level.base_texcoords;
	*cutout_start = level.cutout_start;
	*vertex_count = level.vertex_count;
	*indices = level.indices;
	*triangle_materials = level.triangle_materials;
	*triangle_count = level.triangle_count;
	return level.generation;
}

/* a readable bitmap's average colour (0 to 1): a small mipmap's, at 16
points - the smallest at least 8 pixels across (a compressed one's blocks
are 4); bitmap_2d_get_pixel takes it as a fraction of the mipmaps, rounded
down */
static void level_bitmap_average(struct bitmap_data *bitmap, float *average)
{
	float sum[3] = { 0.0f, 0.0f, 0.0f }, lod = 1.0f;
	long sample, mipmap = 0;

	while (mipmap < bitmap->mipmap_count && (bitmap->width >> (mipmap + 1)) >= 8 &&
		(bitmap->height >> (mipmap + 1)) >= 8)
	{
		mipmap++;
	}
	if (bitmap->mipmap_count > 0)
		lod = 1.0f - ((float)mipmap + 0.25f) / (float)bitmap->mipmap_count;
	/* (the last mipmap: at most all of them - a model's, some, go that far) */
	if (lod < 0.0f)
		lod = 0.0f;
	for (sample = 0; sample < 16; sample++)
	{
		real_point2d point;
		pixel32 pixel;

		point.x = ((float)(sample & 3) + 0.5f) / 4.0f;
		point.y = ((float)(sample >> 2) + 0.5f) / 4.0f;
		pixel = bitmap_2d_get_pixel(bitmap, &point, lod);
		sum[0] += (float)((pixel >> 16) & 0xff) / 255.0f;
		sum[1] += (float)((pixel >> 8) & 0xff) / 255.0f;
		sum[2] += (float)(pixel & 0xff) / 255.0f;
	}
	average[0] = sum[0] / 16.0f;
	average[1] = sum[1] / 16.0f;
	average[2] = sum[2] / 16.0f;
}

/* the materials (RAY_LEVEL_MATERIAL_FLOATS each); TRUE when they changed
since the last call. Each call reads a few more base maps' colours, as the
texture cache loads them. */
boolean halo_ray_tracing_level_materials(const float **materials, long *count)
{
	long step;
	boolean changed;

	for (step = 0; step < 8 && level.material_count > 0; step++)
	{
		long index = level.next_material;
		struct bitmap_data *bitmap = level.base_maps[index];

		level.next_material = (index + 1) % level.material_count;
		/* (asks the cache for them, without waiting) */
		if (bitmap && level_bitmap_readable(bitmap))
		{
			float sum[3];

			level_bitmap_average(bitmap, sum);
			level.materials[index * RAY_LEVEL_MATERIAL_FLOATS + 0] = sum[0];
			level.materials[index * RAY_LEVEL_MATERIAL_FLOATS + 1] = sum[1];
			level.materials[index * RAY_LEVEL_MATERIAL_FLOATS + 2] = sum[2];
			level.base_maps[index] = NULL;
			level.materials_changed = TRUE;
		}
		/* (a self-illuminated one's glow: each colour where its channel of
		the map is, on average - the plasma's half the time) */
		if (level.self_maps[index] && level_bitmap_readable(level.self_maps[index]))
		{
			const float *colors = level.self_colors + index * 9;
			float sum[3];
			long channel;

			level_bitmap_average(level.self_maps[index], sum);
			for (channel = 0; channel < 3; channel++)
			{
				level.materials[index * RAY_LEVEL_MATERIAL_FLOATS + 4 + channel] += RAY_SELF_ILLUMINATION_POWER *
					(colors[channel] * sum[0] + colors[3 + channel] * sum[1] + colors[6 + channel] * sum[2] * 0.5f);
			}
			{
				static long logged;
				const float *glow = level.materials + index * RAY_LEVEL_MATERIAL_FLOATS + 4;

				if (logged++ < 24)
					platform_log("ray tracing: self-illuminated material %ld glows %.2f %.2f %.2f", index, glow[0], glow[1],
						glow[2]);
			}
			level.self_maps[index] = NULL;
			level.materials_changed = TRUE;
		}
	}
	*materials = level.materials;
	*count = level.material_count;
	changed = level.materials_changed;
	level.materials_changed = FALSE;
	return changed;
}

/* how many of the level's lightmap pages are decoded, of how many */
void halo_ray_tracing_level_pages(long *done, long *total)
{
	long index;

	*done = *total = 0;
	for (index = 0; index < level.page_count; index++)
	{
		if (!level.pages[index])
			continue;
		(*total)++;
		if (level.pages_done[index])
			(*done)++;
	}
}

/* the next lightmap page the texture cache has loaded, decoded to RGBA
bytes; FALSE when none is ready */
boolean halo_ray_tracing_level_page(long *page, const unsigned char **pixels, long *width, long *height)
{
	long index;

	for (index = 0; index < level.page_count; index++)
	{
		struct bitmap_data *bitmap = level.pages[index];
		long x, y, size;

		if (level.pages_done[index] || !bitmap)
			continue;
		if (!level_bitmap_readable(bitmap))
			continue;
		size = (long)bitmap->width * bitmap->height * 4;
		if (size > level.page_pixels_size)
		{
			if (level.page_pixels)
				free(level.page_pixels);
			level.page_pixels = malloc((size_t)size);
			level.page_pixels_size = level.page_pixels ? size : 0;
			if (!level.page_pixels)
				return FALSE;
		}
		for (y = 0; y < bitmap->height; y++)
		{
			for (x = 0; x < bitmap->width; x++)
			{
				real_point2d point;
				pixel32 pixel;
				unsigned char *out = level.page_pixels + (y * bitmap->width + x) * 4;

				point.x = ((float)x + 0.5f) / (float)bitmap->width;
				point.y = ((float)y + 0.5f) / (float)bitmap->height;
				pixel = bitmap_2d_get_pixel(bitmap, &point, 1.0f);
				out[0] = (unsigned char)(pixel >> 16);
				out[1] = (unsigned char)(pixel >> 8);
				out[2] = (unsigned char)pixel;
				out[3] = 255;
			}
		}
		level.pages_done[index] = TRUE;
		*page = index;
		*pixels = level.page_pixels;
		*width = bitmap->width;
		*height = bitmap->height;
		return TRUE;
	}
	return FALSE;
}

/* the sky's light: the sun's direction (towards it), its colour times its
power, and the outdoor ambient light's colour times its power (the sky
tag's), then its other lights - the wide ones the lightmaps were lit by, as
the sky's dome - at most two: each its direction, its colour times its
power, the cosine of its half width, and 1 (25 floats); FALSE if the
visible sky has none */
boolean halo_ray_tracing_sky(float *sky)
{
	const struct sky_view *view;
	const byte *raw;
	long index;

	if (render.visible_sky_index == NONE)
		return FALSE;
	view = (const struct sky_view *)scenario_get_sky(render.visible_sky_index);
	if (!view)
		return FALSE;
	raw = (const byte *)view;
	/* (the outdoor ambient radiosity: its colour at 0x48, its power at 0x54) */
	{
		const float *color = (const float *)(raw + 0x48);
		float power = *(const float *)(raw + 0x54);

		sky[6] = color[0] * power;
		sky[7] = color[1] * power;
		sky[8] = color[2] * power;
	}
	sky[0] = sky[1] = sky[2] = sky[3] = sky[4] = sky[5] = 0.0f;
	{
		long fill = 0;

		for (index = 9; index < 25; index++)
			sky[index] = 0.0f;
		for (index = 0; index < view->lights.count && fill < 2; index++)
		{
			const struct sky_light_view *light = (const struct sky_light_view *)view->lights.address + index;
			const float *color = (const float *)((const byte *)light + 0x50);
			float power = *(const float *)((const byte *)light + 0x5C);
			float diameter = *(const float *)((const byte *)light + 0x70);
			float *out = sky + 9 + fill * 8;
			real_vector3d vector;

			if (light->lens_flare.index != NONE)
				continue;
			vector3d_from_euler_angles2d(&vector, &light->direction);
			out[0] = vector.i;
			out[1] = vector.j;
			out[2] = vector.k;
			out[3] = color[0] * power;
			out[4] = color[1] * power;
			out[5] = color[2] * power;
			out[6] = cosf(PIN(diameter, 0.0f, 3.0f) * 0.5f);
			out[7] = 1.0f;
			fill++;
		}
	}
	{
		static const void *logged;

		if (logged != view)
		{
			logged = view;
			for (index = 0; index < view->lights.count; index++)
			{
				const struct sky_light_view *light = (const struct sky_light_view *)view->lights.address + index;
				const float *color = (const float *)((const byte *)light + 0x50);
				real_vector3d vector;

				vector3d_from_euler_angles2d(&vector, &light->direction);
				platform_log("sky light %ld: flags %08lx colour %.2f %.2f %.2f power %.2f dir %.2f %.2f %.2f diameter %.3f flare %d",
					index, *(const unsigned long *)((const byte *)light + 0x4C), color[0], color[1], color[2],
					*(const float *)((const byte *)light + 0x5C), vector.i, vector.j, vector.k,
					*(const float *)((const byte *)light + 0x70), light->lens_flare.index != NONE);
			}
			platform_log("sky: indoor ambient %.2f %.2f %.2f x %.2f, outdoor %.2f %.2f %.2f x %.2f",
				((const float *)(raw + 0x38))[0], ((const float *)(raw + 0x38))[1], ((const float *)(raw + 0x38))[2],
				*(const float *)(raw + 0x44), ((const float *)(raw + 0x48))[0], ((const float *)(raw + 0x48))[1],
				((const float *)(raw + 0x48))[2], *(const float *)(raw + 0x54));
		}
	}
	for (index = 0; index < view->lights.count; index++)
	{
		const struct sky_light_view *light = (const struct sky_light_view *)view->lights.address + index;
		const float *color = (const float *)((const byte *)light + 0x50);
		float power = *(const float *)((const byte *)light + 0x5C);
		real_vector3d vector;

		if (light->lens_flare.index == NONE)
			continue;
		vector3d_from_euler_angles2d(&vector, &light->direction);
		sky[0] = vector.i;
		sky[1] = vector.j;
		sky[2] = vector.k;
		sky[3] = color[0] * power;
		sky[4] = color[1] * power;
		sky[5] = color[2] * power;
		break;
	}
	return TRUE;
}
