/*
RAYTRACE_GL.H

Screen-space ray-traced lighting (raytrace_gl.c, display.ray_tracing).
*/

#ifndef __HALO_RAYTRACE_GL_H
#define __HALO_RAYTRACE_GL_H

/* after a window's opaque world is drawn (source/render/render.c): its
camera's clip planes, vertical field of view (radians), and position,
forward and up vectors in the world (3 floats each) */
void halo_ray_traced_lighting(float z_near, float z_far, float vertical_field_of_view, const float *position,
	const float *forward, const float *up);
/* the light in the window before and after the dynamic lights (0: the
lightmaps, 1: with the flashlight's, the plasma's and the other dynamic
lights): the occlusion darkens only the lightmaps' share; 2: the objects
drawn, before the level (their pixels take the sun's traced shadows) */
void halo_ray_traced_light_stage(int stage);
/* whether the level's lightmaps are left out of the light buffer this
frame (the traced light is added in their place, after the light decals):
the lightmap pass draws the self-illumination alone, and the game's
unshadowed dynamic lights on the level are not drawn (the rays trace them) */
int halo_ray_traced_lightmaps_hidden(void);
/* an object's light, as the game samples it from the lightmap under it
(object_lights.c): the colour, the way it comes from and how much from that
way, replaced by the traced light's probe near the point, if there is one
(and a probe asked for there, for the next frame) */
void halo_ray_traced_object_lighting(const float *position, float *color, float *normal, float *accuracy);
/* after the dynamic lights, before the textures (render.c): the traced light
in place of the lightmaps' (display.ray_tracing_gi); the camera as for
halo_ray_traced_lighting */
void halo_ray_traced_light_buffer(float z_near, float z_far, float vertical_field_of_view, const float *position,
	const float *forward, const float *up);
/* F9 (sdl_platform.c) */
/* returns what it is now, as text */
const char *halo_ray_tracing_toggle(void);
/* F6: the next view (the lighting, the ray view, split, the occlusion);
returns its name */
const char *halo_ray_tracing_next_view(void);
/* F4: the objects' shapes in the rays next (the drawn models, the collision
models, ellipsoids); returns their name */
const char *halo_ray_tracing_shapes_next(void);
/* F5: the ray probe off, live (the crosshair's rays drawn), frozen; returns
what it is now */
const char *halo_ray_tracing_probe_next(void);
/* what it shows: 1 the lighting, 2 the occlusion, 3 the depth, 4 the ray
view, 5 split (tests) */
void halo_ray_tracing_debug_mode(int mode);

/* the settings the overlay changes while playing (settings_overlay.c) */
struct halo_ray_tracing_settings
{
	/* display.ray_tracing: 0 off, 1 on (Metal's rays with the screen's), 2
	the screen's rays only */
	int tracing;
	/* what it shows (F6): 0 the lighting, 1 the ray view, 2 split, 3 the
	occlusion, 4 the depth */
	int view;
	/* display.ray_tracing_gi: 0 off, 1 traced, 2 black, 3 path */
	int gi;
	/* display.ray_tracing_lights "traced"; display.ray_tracing_shapes (0
	model, 1 collision); display.ray_tracing_objects; _gi_split */
	int traced_lights, shapes, objects, gi_split;
	/* the strengths */
	float occlusion, reflections, bounce, shadows, gi_sun, gi_bounce, gi_glow, gi_lights;
	/* the path tracer's bounces at most (1-4), the traced light's rays a
	pixel (1-8) */
	float gi_bounces, gi_samples;
	/* (read only) whether Metal's rays can be had, and whether the ray
	tracing could not start */
	int hardware_available, failed;
};
void halo_ray_tracing_get(struct halo_ray_tracing_settings *settings);
/* takes them up from the next frame (the traced light gathered again when
what it traces changes) */
void halo_ray_tracing_set(const struct halo_ray_tracing_settings *settings);

#endif
