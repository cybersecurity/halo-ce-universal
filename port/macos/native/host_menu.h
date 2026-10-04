#ifndef HALO_MAC_MENU_H
#define HALO_MAC_MENU_H
#include <stddef.h>
void host_menu_initialize_application(void);
int host_menu_prepare(const char *support, const char *fallback, char *data, size_t capacity);
void host_menu_begin_game(void);
void host_menu_finish_game(int exit_code);
/* SDL operations stay on its real main thread. */
int host_sdl_is_fullscreen(void);
int host_sdl_display_mode(void);
int host_sdl_apply_display(int mode, int width, int height);
void host_sdl_release_mouse(void);
void host_sdl_request_quit(void);
#endif
