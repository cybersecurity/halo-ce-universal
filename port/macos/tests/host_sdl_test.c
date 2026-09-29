#include "guest_address.h"
#include "host_sdl.h"

#include <SDL3/SDL.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

int main(void)
{
	const uint32_t guest_base = 0x30000000u;
	const size_t guest_size = 0x4000;
	void *guest_memory = mmap(NULL, guest_size, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	SDL_Event event;
	SDL_Event *guest_event;

	assert(guest_memory != MAP_FAILED);
	assert(mac_guest_address_register(guest_base, guest_memory, guest_size) == 0);
	assert(mac_host_sdl_init(SDL_INIT_VIDEO | SDL_INIT_EVENTS));

	/* Window objects remain tokens; logical dimensions are separate from
	 * Retina pixel dimensions and bad guest outputs must fail unchanged. */
	strcpy(guest_memory, "Halo bridge test");
	uint32_t window = mac_host_sdl_create_window(guest_base, 640, 480, SDL_WINDOW_HIDDEN);
	assert(window != 0);
	uint32_t width_va = guest_base + 256, height_va = guest_base + 260;
	int *width = mac_guest_address_resolve(width_va, sizeof(*width));
	int *height = mac_guest_address_resolve(height_va, sizeof(*height));
	assert(mac_host_sdl_window_size(window, width_va, height_va));
	assert(*width == 640 && *height == 480);
	assert((mac_host_sdl_window_flags(window) & SDL_WINDOW_HIDDEN) != 0);
	assert(!mac_host_sdl_set_window_size(0,1280,720));
	assert(!mac_host_sdl_set_window_size(window,0,720));
	assert(!mac_host_sdl_set_window_size(window,1280,-1));
	const int sizes[][2]={{1280,720},{1920,1080},{640,480}};
	for(unsigned i=0;i<3;i++) {
		assert(mac_host_sdl_set_window_size(window,sizes[i][0],sizes[i][1]));
		SDL_PumpEvents();
		assert(mac_host_sdl_window_size(window,width_va,height_va));
		assert(*width==sizes[i][0] && *height==sizes[i][1]);
		assert(mac_host_sdl_window_size_in_pixels(window,width_va,height_va));
		assert(*width>0 && *height>0);
	}
	*width = 123;
	assert(!mac_host_sdl_window_size(window, width_va, 0));
	assert(*width == 123);
	assert(!mac_host_sdl_window_size(0, width_va, height_va));
	assert(mac_host_sdl_window_flags(0) == 0);
	assert(!mac_host_sdl_set_window_fullscreen(0, 1));
	assert(!mac_host_sdl_warp_mouse(0, 320.0f, 240.0f));
	assert(mac_host_sdl_set_window_fullscreen(window, 1));
	assert((mac_host_sdl_window_flags(window) & SDL_WINDOW_FULLSCREEN) != 0);
	assert(mac_host_sdl_set_window_fullscreen(window, 0));
	assert((mac_host_sdl_window_flags(window) & SDL_WINDOW_FULLSCREEN) == 0);
	assert(mac_host_sdl_warp_mouse(window, 320.5f, 240.25f));
	/* Window creation queues unrelated events before the fixture below. */
	while (SDL_PollEvent(&event)) {}

	/* Pointer-bearing user events are discarded; the next supported event is
	 * copied into guest memory with native pointer fields never exposed. */
	memset(&event, 0, sizeof(event));
	event.type = SDL_EVENT_USER;
	event.user.data1 = (void *)(uintptr_t)0x12345678;
	assert(SDL_PushEvent(&event));
	memset(&event, 0, sizeof(event));
	event.type = SDL_EVENT_KEY_DOWN;
	event.key.key = SDLK_RETURN;
	event.key.down = true;
	assert(SDL_PushEvent(&event));

	assert(mac_host_sdl_poll_event(guest_base));
	guest_event = mac_guest_address_resolve(guest_base, sizeof(*guest_event));
	assert(guest_event);
	assert(guest_event->type == SDL_EVENT_KEY_DOWN);
	assert(guest_event->key.key == SDLK_RETURN);
	assert(guest_event->key.down);

	SDL_Quit();
	mac_guest_address_reset();
	puts("host_sdl_test: ok");
	return 0;
}
