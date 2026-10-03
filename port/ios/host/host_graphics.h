#pragma once
#include <stdint.h>
struct SDL_Window;
void halo_graphics_initialize(void);
int halo_graphics_metal(void);
int halo_graphics_prefer_metal(void);
void halo_graphics_set_preference(int metal);
void *halo_graphics_proc(const char *name);
int halo_graphics_has_extension(const char *name);
void *halo_graphics_create(struct SDL_Window *window);
int halo_graphics_make_current(void);
int halo_graphics_swap(void);
int halo_graphics_swap_interval(int interval);
/* IOSurface ownership is transferred to/from ANGLE around each MetalFX copy. */
unsigned int halo_graphics_shared_create(void *iosurface,unsigned int width,unsigned int height);
int halo_graphics_shared_begin(void);
int halo_graphics_shared_end(void);
void halo_graphics_shared_destroy(void);
