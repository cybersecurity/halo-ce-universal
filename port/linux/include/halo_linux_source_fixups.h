/*
HALO_LINUX_SOURCE_FIXUPS.H

Game-only workarounds for source that MSVC accepts but clang rejects, where
editing the source itself would change the byte-matched MSVC output (see
port/linux/README.md for how each was checked).
*/

#ifndef __HALO_LINUX_SOURCE_FIXUPS_H
#define __HALO_LINUX_SOURCE_FIXUPS_H

/* rasterizer.h declares rasterizer_debug_drawing_begin(boolean opaque) while
rasterizer_xbox_debug.h declares a second `long zbias` parameter, and
rasterizer_debug.c includes both and passes two arguments. MSVC tolerates
the mismatch; the definition ignores zbias. Adding the parameter to
rasterizer.h perturbs MSVC's register allocation elsewhere, so instead every
declaration and call collapses to the one-parameter form here. */
#define rasterizer_debug_drawing_begin(opaque, ...) (rasterizer_debug_drawing_begin)(opaque)

/* frames between the 30 Hz ticks (port/linux/game/render_interpolation.c);
the platform layer reads the display.interpolation setting */
struct observer_result;
struct render_camera;
struct real_matrix4x3;
int halo_interpolation_enabled(void);
float game_time_get_tick_fraction(void);
void render_interpolation_tick(void);
void render_interpolation_reset(void);
void render_interpolation_frame_begin(void);
void render_interpolation_frame_end(void);
float render_interpolation_fraction(void);
struct real_matrix4x3 *render_interpolation_object_node_matrices(long object_index);
struct observer_result const *render_interpolation_camera(short local_player_index,
	struct observer_result const *observer);
void render_interpolation_first_person(short local_player_index, struct real_matrix4x3 *node_matrices,
	short node_count, struct render_camera const *camera);
float render_interpolation_game_time_sec(long ticks);

/* the width of the screen the game draws, 480 lines tall: the device's or
the display's shape, or 640 (port/linux/src/d3d8_gl.c) */
long halo_screen_width(void);
/* the screen's pixels to the Xbox's one (port/linux/src/d3d8_gl.c) */
float halo_screen_scale(void);
/* takes up a new width between frames (F11); returns the width */
long halo_screen_commit(void);
/* while TRUE, drawing shifts right to center 640-column layouts */
void halo_screen_ui_offset(unsigned char centered);
/* screen-space ray-traced lighting, after a window's opaque world
(port/linux/src/raytrace_gl.c) */
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
/* the console command that starts game.map (HALO_MAP), or NULL
(port/linux/src/port_config.c) */
const char *halo_startup_map_command(void);
/* the next of debug.commands (HALO_COMMANDS) whose time has come, or NULL
(port/linux/src/port_config.c) */
const char *halo_timed_command_next(void);
/* the mouse in the menus (source/interface/ui_widget.c) */
#include "halo_ui_pointer.h"

#endif
