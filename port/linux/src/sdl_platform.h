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
/* whether the window is fullscreen now: display.fullscreen at first, then as
F11 and VIDEO SETTINGS (game_settings.c) switch it */
BOOL platform_fullscreen(void);
/* fullscreen or the window (SDL keeps the window's size and place while
fullscreen); with wait, once the switch is done. FALSE if SDL refuses it */
BOOL platform_set_fullscreen(BOOL fullscreen, BOOL wait);
/* the largest display.window_scale whose window fits the display */
int platform_window_scale_maximum(void);
/* resizes the window now, or when it next leaves fullscreen; FALSE if SDL
refuses */
BOOL platform_set_window_scale(int scale);
/* the resolution the game draws at while fullscreen, in pixels
(display.resolution_width and _height: a size of the display's, or the
display's own); d3d8_gl.c scales the picture to the display */
void platform_fullscreen_resolution(int *width, int *height);
/* the sizes of the display's the game can draw at fullscreen, each once:
the display's own first, then the rest, largest first; how many there are */
int platform_display_resolutions(int *widths, int *heights, int maximum);
/* draws at the size from the next frame on (while fullscreen); FALSE if the
display has no room for it */
BOOL platform_set_fullscreen_resolution(int width, int height);
#endif
/* display.window_scale as it is now: the window's size as a multiple of
640x480 */
int platform_window_scale(void);
void platform_video_drawable_size(int *width, int *height);
void platform_video_swap(void);
/* whether frames wait for the display (display.vsync), as the driver has it */
BOOL platform_vsync(void);
/* FALSE if the driver refuses, which leaves it as it was */
BOOL platform_set_vsync(BOOL vsync);
/* frames between the 30 Hz ticks at the display's refresh rate, unless
display.interpolation is false (port/linux/game/render_interpolation.c) */
int halo_interpolation_enabled(void);
void halo_interpolation_set_enabled(int enabled);
void platform_mouse_capture(BOOL capture);

/* main thread only; a no-op elsewhere */
void platform_pump_events(void);
/* a snapshot of the input state; consume_motion resets the mouse deltas */
void platform_input_read(struct platform_input_state *state, BOOL consume_motion);
/* input.mouse_sensitivity, input.invert_mouse and input.mouse_aim_assist as
the mouse aim has them (xinput_sdl.c) */
float input_mouse_sensitivity(void);
void input_set_mouse_sensitivity(float sensitivity);
int input_mouse_inverted(void);
void input_set_mouse_inverted(int inverted);
int input_mouse_aim_assist(void);
void input_set_mouse_aim_assist(int assisted);
/* audio.enabled (whether a device plays the sound) and audio.volume as the
mixer has them (dsound_sdl.c) */
BOOL audio_output_enabled(void);
BOOL audio_set_output_enabled(BOOL enabled);
float audio_master_volume(void);
void audio_set_master_volume(float volume);
/* display.anisotropic_filtering as the renderer has it (d3d8_gl.c): the
level (1 for none), the most the GPU filters with, and a level for the
textures drawn from now on (more than the most is the most) */
int video_anisotropic_filtering(void);
int video_anisotropic_filtering_maximum(void);
void video_set_anisotropic_filtering(int level);
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

#endif
