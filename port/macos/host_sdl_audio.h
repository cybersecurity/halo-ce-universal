#ifndef HALO_MACOS_HOST_SDL_AUDIO_H
#define HALO_MACOS_HOST_SDL_AUDIO_H

#include <stdint.h>

/* Audio is opt-in until the translated callback path has been exercised on
 * the target game/runtime: set HALO_AUDIO_ENABLE=1 to enable it. */
int mac_host_sdl_audio_enabled(void);
uint32_t mac_host_sdl_open_audio_stream(uint32_t device, uint32_t spec_va,
	uint32_t callback_va, uint32_t userdata_va);
int mac_host_sdl_put_audio_stream_data(uint32_t stream_handle,
	uint32_t data_va, int length);
int mac_host_sdl_resume_audio_stream_device(uint32_t stream_handle);
void mac_host_sdl_audio_shutdown(void);

#endif
