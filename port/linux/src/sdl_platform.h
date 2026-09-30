/*
SDL_PLATFORM.H

Window, OpenGL context and input state shared by the renderer and the
controller emulation (see sdl_platform.c).
*/

#ifndef __HALO_LINUX_SDL_PLATFORM_H
#define __HALO_LINUX_SDL_PLATFORM_H

#include <SDL3/SDL_scancode.h>

#define PLATFORM_MOUSE_BUTTON_COUNT 8

struct platform_input_state
{
	unsigned char keys[SDL_SCANCODE_COUNT];
	unsigned char mouse_buttons[PLATFORM_MOUSE_BUTTON_COUNT]; /* SDL_BUTTON_* */
	float mouse_dx, mouse_dy;
	float mouse_wheel;
	BOOL focused;
	BOOL mouse_released;
	/* the mouse drives the menus' pointer (platform_ui_pointer_set_active)
	instead of the controller */
	BOOL ui_pointer;
};

struct platform_keystroke
{
	BYTE virtual_key;
	CHAR ascii;
	BYTE flags;
};

BOOL platform_sdl_initialize(void);
/* creates the window and makes its OpenGL context current on this thread */
BOOL platform_video_initialize(unsigned long width, unsigned long height);
#ifndef HALO_ANDROID
BOOL platform_screen_mode(long *width, long *height);
BOOL platform_output_size(long *width, long *height);
#endif
/* F8: the next of the resolutions (d3d8_gl.c, display.resolution) */
const char *halo_screen_resolution_next(void);
#ifndef HALO_ANDROID
/* the resolution now (F8's or display.resolution), and the settings
overlay's choice of it (d3d8_gl.c) */
const char *halo_screen_resolution_current(void);
void halo_screen_resolution_set(const char *name);
/* whether the window is fullscreen; 0 or 1 makes it so first (F11's), -1
only asks (a hidden window stays a window) */
int platform_window_fullscreen(int fullscreen);
/* waits for the display between frames, or not (display.vsync) */
void platform_video_set_vsync(int vsync);
#endif
void platform_video_drawable_size(int *width, int *height);
void platform_video_swap(void);
/* frames between the 30 Hz ticks at the display's refresh rate, unless
display.interpolation is false (port/linux/game/render_interpolation.c) */
int halo_interpolation_enabled(void);
void platform_mouse_capture(BOOL capture);

/* main thread only; a no-op elsewhere */
void platform_pump_events(void);
/* a snapshot of the input state; consume_motion resets the mouse deltas */
void platform_input_read(struct platform_input_state *state, BOOL consume_motion);
#ifndef HALO_ANDROID
/* the pointer in the menus (d3d8_gl.c, halo_ui_pointer_update) */
struct platform_ui_pointer
{
	/* in window coordinates, as SDL reports them */
	float x, y;
	float click_x, click_y;
	BOOL moved;
	int left_clicks, right_clicks;
	int wheel_steps;
};
void platform_ui_pointer_set_active(BOOL active);
BOOL platform_ui_pointer_read(struct platform_ui_pointer *pointer);
void platform_video_window_size(int *width, int *height);
#endif
BOOL platform_next_keystroke(struct platform_keystroke *keystroke);
/* Command-X (switch_camera) and Command-Z: the debug cameras (xinput_sdl.c) */
void halo_debug_camera_key(int switch_camera);

#endif
