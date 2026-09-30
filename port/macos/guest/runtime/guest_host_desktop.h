/*
GUEST_HOST_DESKTOP.H

The host services the macOS guest imports beyond the Android port's
(port/android/guest/runtime/guest_host.h), for the desktop's SDL functions
(guest_sdl_desktop.c). Every name here must also be listed in
port/macos/host_imports.list; tools/macos_host_thunks.py reads the
prototypes. Parameter types follow the rules of guest_host.h.
*/

#ifndef __GUEST_HOST_DESKTOP_H
#define __GUEST_HOST_DESKTOP_H

void host_sdl_destroy_window(unsigned int window);
long long host_sdl_get_window_flags(unsigned int window);
int host_sdl_set_window_fullscreen(unsigned int window, int fullscreen);
int host_sdl_get_window_size(unsigned int window, int *width, int *height);
void host_sdl_warp_mouse_in_window(unsigned int window, float x, float y);
unsigned int host_sdl_get_primary_display(void);
unsigned int host_sdl_get_display_for_window(unsigned int window);
int host_sdl_get_desktop_display_mode(unsigned int display, int *fields, float *reals);
int host_sdl_wait_event_timeout(void *event, int milliseconds);
unsigned int host_sdl_create_renderer(unsigned int window);
void host_sdl_destroy_renderer(unsigned int renderer);
int host_sdl_set_render_vsync(unsigned int renderer, int vsync);
int host_sdl_set_render_draw_color(unsigned int renderer, unsigned int r, unsigned int g, unsigned int b,
	unsigned int a);
int host_sdl_set_render_scale(unsigned int renderer, float x, float y);
int host_sdl_render_clear(unsigned int renderer);
int host_sdl_render_fill_rect(unsigned int renderer, const float *rectangle);
int host_sdl_render_debug_text(unsigned int renderer, float x, float y, const char *text);
int host_sdl_render_present(unsigned int renderer);
int host_sdl_show_message_box(unsigned int flags, const char *title, const char *message, int count,
	const int *button_flags, const int *button_ids, const char *texts, int *chosen);
void host_sdl_show_open_file_dialog(const char *filters, int count);
int host_sdl_file_dialog_result(char *buffer, unsigned int size);
/* Metal ray tracing (port/macos/host/host_metal_rt.m) */
int host_rt_available(void);
int host_rt_set_world(unsigned int generation, const float *vertices, int vertex_count, const unsigned int *indices,
	int triangle_count);
unsigned int host_rt_texture(int which, int width, int height);
int host_rt_trace(const float *camera, int width, int height);
void host_rt_set_objects(const float *triangles, const unsigned char *groups, const float *cutouts, int count,
	int two_sided);
int host_rt_probe(float *segments, int maximum);
void host_rt_set_lights(const float *lights, int count);
void host_rt_set_emitters(const float *emitters, int count);
int host_rt_set_level(unsigned int generation, const float *vertices, const float *texcoords,
	const float *base_texcoords, int vertex_count, const unsigned int *indices, const unsigned int *triangle_materials,
	int triangle_count, int cutout_start);
int host_rt_set_mask(unsigned int generation, int index, int width, int height, const unsigned char *alpha);
void host_rt_set_level_materials(const float *materials, int count);
int host_rt_set_level_page(int page, int width, int height, const unsigned char *pixels);
void host_rt_set_probes(const float *points, int count);
int host_rt_probe_results(float *results, int maximum);

#endif
