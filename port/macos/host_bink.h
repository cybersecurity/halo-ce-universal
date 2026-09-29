#ifndef HALO_HOST_BINK_H
#define HALO_HOST_BINK_H
#include <stdint.h>
#include <stddef.h>
struct halo_bink;
struct halo_bink_info { uint32_t width, height, frames, frame_num, last_frame_num; };
struct halo_bink_evidence { uint64_t video_frames, pcm_samples, nonzero_pcm_samples; double pcm_peak; uint64_t copied_frames, presented_frames; int audio_drained; };
struct halo_bink *mac_bink_open_path(const char *path, int audio_output);
int mac_bink_decode(struct halo_bink *movie);
void mac_bink_next(struct halo_bink *movie);
int mac_bink_wait(struct halo_bink *movie);
const struct halo_bink_info *mac_bink_info(const struct halo_bink *movie);
const unsigned char *mac_bink_pixels(const struct halo_bink *movie);
const struct halo_bink_evidence *mac_bink_evidence(const struct halo_bink *movie);
int mac_bink_start_playback(struct halo_bink *movie);
int mac_bink_audio_pending(const struct halo_bink *movie);
void mac_bink_close(struct halo_bink *movie);
uint32_t mac_host_bink_open(uint32_t path_va, uint32_t info_va, int audio_enabled);
int mac_host_bink_decode(uint32_t token);
int mac_host_bink_next(uint32_t token, uint32_t info_va);
int mac_host_bink_wait(uint32_t token);
int mac_host_bink_copy(uint32_t token, uint32_t destination_va, int32_t pitch,
	uint32_t height, uint32_t x, uint32_t y, uint32_t flags);
void mac_host_bink_close(uint32_t token);
void mac_host_bink_presented(void);
#endif
