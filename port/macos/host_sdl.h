#ifndef HALO_MACOS_HOST_SDL_H
#define HALO_MACOS_HOST_SDL_H

#include <stdint.h>

/* Darwin host implementations of the upstream Android host_sdl_* imports.
 * All string/buffer/event pointers are 32-bit guest virtual addresses. */
int mac_host_sdl_init(uint32_t flags);
int mac_host_sdl_set_hint(uint32_t name_va, uint32_t value_va);
int mac_host_sdl_get_error(uint32_t buffer_va, uint32_t size);
int64_t mac_host_sdl_ticks(void);
int64_t mac_host_sdl_thread_id(void);
uint32_t mac_host_sdl_create_window(uint32_t title_va, int width, int height, int64_t flags);
int mac_host_sdl_window_size_in_pixels(uint32_t window, uint32_t width_va, uint32_t height_va);
int mac_host_sdl_window_size(uint32_t window, uint32_t width_va, uint32_t height_va);
int64_t mac_host_sdl_window_flags(uint32_t window);
int mac_host_sdl_set_window_fullscreen(uint32_t window, int enabled);
int mac_host_sdl_set_window_size(uint32_t window, int width, int height);
int mac_host_sdl_warp_mouse(uint32_t window, float x, float y);
int mac_host_sdl_platform_screen_mode(uint32_t width_va, uint32_t height_va);
int mac_host_sdl_set_relative_mouse(uint32_t window, int enabled);
int mac_host_sdl_gl_set_attribute(int attribute, int value);
uint32_t mac_host_sdl_gl_create_context(uint32_t window);
int mac_host_sdl_gl_make_current(uint32_t window, uint32_t context);
int mac_host_sdl_gl_set_swap_interval(int interval);
int mac_host_sdl_gl_swap_window(uint32_t window);
int mac_host_sdl_poll_event(uint32_t event_va);
int mac_host_sdl_set_clipboard_text(uint32_t text_va);
int mac_host_sdl_get_clipboard_text(uint32_t buffer_va, uint32_t size);
int mac_host_sdl_show_toast(uint32_t message_va, int duration, int gravity, int x, int y);
int mac_host_sdl_show_simple_message_box(uint32_t flags, uint32_t title_va, uint32_t message_va);
int mac_host_sdl_get_gamepads(uint32_t ids_va, int capacity);
uint32_t mac_host_sdl_open_gamepad(uint32_t id);
uint32_t mac_host_sdl_gamepad_from_id(uint32_t id);
int mac_host_sdl_gamepad_axis(uint32_t gamepad, int axis);
int mac_host_sdl_gamepad_button(uint32_t gamepad, int button);
int mac_host_sdl_gamepad_type(uint32_t gamepad);
int mac_host_sdl_rumble_gamepad(uint32_t gamepad, uint32_t low, uint32_t high, uint32_t milliseconds);

#endif
