#include "host_bink.h"
#include <assert.h>
#include <stdio.h>
#include <SDL3/SDL.h>
#include "guest_address.h"
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
	if (argc != 2) { fprintf(stderr,"usage: host_bink_codec_test intro.bik\n"); return 2; }
	assert(SDL_Init(SDL_INIT_EVENTS));
	struct halo_bink *movie = mac_bink_open_path(argv[1], 0);
	assert(movie);
	const struct halo_bink_info *info = mac_bink_info(movie);
	assert(info->width == 640 && info->height == 480 && info->frames > 400);
	unsigned frames = 0;
	uint64_t colored_bytes = 0;
	while (mac_bink_decode(movie))
	{
		const unsigned char *pixels = mac_bink_pixels(movie);
		for (size_t i = 0; i < (size_t)info->width * info->height * 4; i += 4)
			colored_bytes += pixels[i] != 0 || pixels[i+1] != 0 || pixels[i+2] != 0;
		++frames; mac_bink_next(movie);
	}
	const struct halo_bink_evidence *evidence = mac_bink_evidence(movie);
	assert(frames == info->frames && colored_bytes > 0);
	assert(evidence->pcm_samples > 1000000 && evidence->nonzero_pcm_samples > 0);
	/* Lossy Bink reconstruction can overshoot full scale; SDL handles output
	 * clipping. Require finite, nonzero, bounded decoder output here. */
	assert(evidence->pcm_peak > 0 && evidence->pcm_peak < 4);
	printf("native intro decode: frames=%u colored_pixels=%llu PCM_samples=%llu nonzero=%llu peak=%.6f\n",
		frames,(unsigned long long)colored_bytes,(unsigned long long)evidence->pcm_samples,
		(unsigned long long)evidence->nonzero_pcm_samples,evidence->pcm_peak);
	mac_bink_close(movie);
	/* Exercise the exact five-DWORD guest wire and pitched texture transfer. */
	const uint32_t base = 0x30000000;
	const size_t guest_size = 2 * 1024 * 1024;
	unsigned char *guest = calloc(1, guest_size);
	assert(guest && strlen(argv[1]) < 1024);
	assert(mac_guest_address_register(base, guest, guest_size) == 0);
	strcpy((char *)guest, argv[1]);
	memset(guest + 1024, 0xa5, 32);
	assert(!mac_host_bink_open(base, base + (uint32_t)guest_size - 10, 0));
	uint32_t token = mac_host_bink_open(base, base + 1024, 0);
	assert(token > 0 && token < 8);
	uint32_t wire[5]; memcpy(wire, guest + 1024, sizeof(wire));
	assert(wire[0] == 640 && wire[1] == 480 && wire[2] == 481 && wire[3] == 0 && wire[4] == 0);
	for (unsigned i = 1044; i < 1056; ++i) assert(guest[i] == 0xa5);
	assert(mac_host_bink_decode(token));
	assert(mac_host_bink_next(token, base + 1024));
	memcpy(wire, guest + 1024, sizeof(wire));
	assert(wire[3] == 0 && wire[4] == 0);
	assert(mac_host_bink_wait(token) == 0); /* First frame has PTS zero. */
	const uint32_t output_va = base + 4096;
	const uint32_t pitch = 644 * 4;
	memset(guest + 4096, 0xa5, (size_t)pitch * 482);
	assert(mac_host_bink_copy(token, output_va, (int32_t)pitch, 482, 2, 1, 0x80000003));
	for (unsigned i = 0; i < pitch; ++i) assert(guest[4096 + i] == 0xa5);
	for (unsigned row = 1; row <= 480; ++row)
	{
		unsigned char *pixels = guest + 4096 + row * pitch;
		for (unsigned i = 0; i < 8; ++i) assert(pixels[i] == 0xa5);
		for (unsigned i = 2568; i < pitch; ++i) assert(pixels[i] == 0xa5);
	}
	assert(!mac_host_bink_copy(token, output_va, 2560, 480, 1, 0, 3));
	assert(!mac_host_bink_copy(token, output_va, -2560, 480, 0, 0, 3));
	assert(!mac_host_bink_copy(token, output_va, 2560, 479, 0, 0, 3));
	assert(!mac_host_bink_copy(token, base + (uint32_t)guest_size - 10, 2560, 480, 0, 0, 3));
	assert(!mac_host_bink_copy(token, output_va, 2560, 480, 0, 0, 2));
	mac_host_bink_presented();
	/* Mirror the guest's DoFrame/Next/Copy/Present/end predicate exactly.
	 * Every one of its 481 source frames must survive that predicate. */
	unsigned presented = 1;
	while (wire[3] != wire[2] - 1)
	{
		assert(mac_host_bink_decode(token));
		assert(mac_host_bink_next(token, base + 1024));
		assert(mac_host_bink_copy(token, output_va, (int32_t)pitch, 482, 2, 1, 0x80000003));
		memcpy(wire, guest + 1024, sizeof(wire));
		++presented;
		if (wire[3] == wire[2] - 1) mac_host_bink_close(token);
		mac_host_bink_presented();
	}
	assert(presented == 481);
	mac_host_bink_close(token); mac_host_bink_close(token);
	assert(!mac_host_bink_decode(token));
	assert(!mac_host_bink_next(token, base + 1024));
	mac_guest_address_reset(); free(guest);
	/* Native audio submission/close is checked against SDL's dummy device;
	 * the codec fixture never plays through the user's speakers. */
	assert(SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy"));
	assert(SDL_InitSubSystem(SDL_INIT_AUDIO));
	movie = mac_bink_open_path(argv[1], 1);
	assert(movie);
	/* Full soundtrack is ready while the device remains paused before the
	 * first video frame. Slow video decode cannot starve this queue. */
	assert(mac_bink_evidence(movie)->pcm_samples == 1542270);
	assert(mac_bink_audio_pending(movie) == 1542270 * (int)sizeof(float));
	for (unsigned i = 0; i < 240; ++i) { assert(mac_bink_decode(movie)); mac_bink_next(movie); }
	assert(mac_bink_evidence(movie)->nonzero_pcm_samples > 0);
	assert(mac_bink_start_playback(movie));
	uint64_t audio_deadline = SDL_GetTicks() + 18000;
	while (mac_bink_audio_pending(movie) > 0 && SDL_GetTicks() < audio_deadline) SDL_Delay(10);
	assert(mac_bink_audio_pending(movie) == 0);
	assert(mac_bink_evidence(movie)->pcm_samples == 1542270);
	puts("native BINK dummy device consumed all 1542270 PCM samples without video progression");
	mac_bink_close(movie);
	SDL_Quit(); puts("native BINK five-DWORD wire, timing, pitched copy, bad bounds and close passed"); return 0;
}
