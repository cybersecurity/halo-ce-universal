/*
RASTERIZER_TRANSPARENT_GEOMETRY.H

Narrow cross-translation-unit interface owned by RASTERIZER_TRANSPARENT_GEOMETRY.C.
*/

#ifndef __RASTERIZER_TRANSPARENT_GEOMETRY_H
#define __RASTERIZER_TRANSPARENT_GEOMETRY_H
#pragma once

#include "cseries.h"
#ifdef HALO_64BIT
#include "rasterizer/rasterizer_model_types.h"
#endif

#ifdef HALO_64BIT
struct triangle_buffer;
struct vertex_buffer;
struct bitmap_data;
struct render_lighting;
struct render_animation;

/* One definition for every unit. The decompiled units each described the
parts they use, padding the rest to January's 32-bit layout; with 64-bit
pointers those copies disagree. Fields that units named or typed
differently share a union. */
struct transparent_geometry_group
{
	unsigned int geometry_flags;
	int object_index;
	int source_object_index;
	struct shader *shader;
	short shader_permutation_index;
	word pad12;
	union
	{
		struct rasterizer_model_effect_parameters effect;
		struct
		{
			short effect_type;
			short pad16;
			real effect_intensity;
		};
	};
	real_vector2d model_base_map_scale;
	int dynamic_triangle_buffer_index;
	union
	{
		struct triangle_buffer const *triangle_buffer;
		void (*render_proc)(int object_index, int widget_index);
	};
	int first_triangle_index;
	int triangle_count;
	int dynamic_vertex_buffer_index;
	union
	{
		struct vertex_buffer const *vertex_buffer;
		struct vertex_buffer const *vertex_buffers;
	};
	struct bitmap_data const *lightmap;
	real_matrix4x3 const *node_matrices;
	short node_matrix_count;
	word pad66;
	struct render_lighting const *lighting;
	struct render_animation const *animation;
	real z_sort;
	real_point3d centroid;
	real_plane3d plane;
	int sorted_index;
	short previous_group_presorted_index;
	short next_group_presorted_index;
	int active_camouflage_transparent_source_object_index;
	boolean sort_last;
	boolean cortana_hack;
	byte pad9E[2];
};
#else
struct transparent_geometry_group;
#endif

void rasterizer_transparent_geometry_groups_begin(
	void);
void rasterizer_transparent_geometry_groups_end(
	void);
void rasterizer_transparent_geometry_group_draw(
	struct transparent_geometry_group *group,
	boolean dirty);
void rasterizer_transparent_geometry_group_draw__internal(
	struct transparent_geometry_group const *group,
	boolean has_lightmap);

#endif /* __RASTERIZER_TRANSPARENT_GEOMETRY_H */
