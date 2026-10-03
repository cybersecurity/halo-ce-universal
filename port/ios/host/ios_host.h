/* iOS host services for the ILP32 guest in an aligned native address arena. */
#ifndef HALO_IOS_HOST_H
#define HALO_IOS_HOST_H
#include "host.h"
#include <stdint.h>
extern uintptr_t host_arena;
static inline void *host_pointer(uint64_t p) { return p ? (void *)(host_arena | (uint32_t)p) : NULL; }
static inline uint32_t guest_pointer(const void *p) { return (uint32_t)(uintptr_t)p; }
uint32_t host_ios_default_framebuffer(void);
struct SDL_Window;
struct SDL_Gamepad;
void host_ios_touch_initialize(void);
void host_ios_prepare_assets(const char *documents);
/* tvOS: receive the player's XISO over the local network into <documents>/maps. */
void host_tv_import(const char *documents);
void host_ios_touch_attach(struct SDL_Window *window);
void host_ios_touch_reset(void);
int host_ios_gamepads(uint32_t *out, int capacity);
int host_ios_gamepad_type(struct SDL_Gamepad *pad);
int host_ios_gamepad_axis(struct SDL_Gamepad *pad, int axis);
int host_ios_gamepad_button(struct SDL_Gamepad *pad, int button);
int host_linux_errno(int native_errno);
void host_install_signal_handlers(void);
uint32_t host_guest_invoke(uintptr_t function, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uintptr_t arena);
uint32_t host_guest_on_stack(uintptr_t function, uint32_t argument, uintptr_t arena, void *stack_top);
#endif
