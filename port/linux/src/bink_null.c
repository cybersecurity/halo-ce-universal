/*
BINK_NULL.C

The Bink video SDK entry points bink_playback.c uses. There is no Bink
decoder in the Linux build (the RAD SDK is proprietary and the reconstructed
libs/binkxbox is incomplete), so BinkOpen reports that a movie cannot be
opened and the game skips it, exactly as it does for a missing movie file.

The prototypes match the declarations in bink_playback.c; the RAD SDK's
RADEXPLINK is __stdcall.
*/

#include "platform.h"
#ifdef HALO_MACOS
#include "guest_host.h"
#include "port_config.h"
#include <string.h>
#endif

typedef void *(__stdcall *rad_memory_allocate_proc)(unsigned long size);
typedef void (__stdcall *rad_memory_free_proc)(void *memory);
typedef void *(__stdcall *bink_sound_system_open_proc)(unsigned long param);
typedef struct BINK *HBINK;
#ifdef HALO_MACOS
struct BINK
{
	unsigned long Width, Height, Frames, FrameNum, LastFrameNum;
	unsigned int native_token;
};
typedef char bink_wire_size_must_be_24[sizeof(struct BINK) == 24 ? 1 : -1];
#endif

void __stdcall RADSetMemory(rad_memory_allocate_proc allocate, rad_memory_free_proc release)
{
	(void)allocate;
	(void)release;
}

void *__stdcall BinkOpenDirectSound(unsigned long param)
{
#ifdef HALO_MACOS
	return (void *)param;
#else
	(void)param;
	return NULL;
#endif
}

long __stdcall BinkSetSoundSystem(bink_sound_system_open_proc open, unsigned long param)
{
	(void)open;
	(void)param;
	return
#ifdef HALO_MACOS
		1;
#else
		0;
#endif
}

void __stdcall BinkSetIOSize(unsigned long io_size)
{
	(void)io_size;
}

HBINK __stdcall BinkOpen(const char *name, unsigned long flags)
{
	(void)flags;
#ifdef HALO_MACOS
	char path[1024];
	HBINK movie = calloc(1, sizeof(*movie));
	if (!movie) return NULL;
	platform_translate_path(name, path, sizeof(path));
	movie->native_token = host_bink_open(path, movie, config_boolean("audio.enabled"));
	if (!movie->native_token) { free(movie); platform_log("native Bink cannot open %s", path); return NULL; }
	return movie;
#else
	platform_log("Bink video is not supported; skipping \"%s\"", name ? name : "");
	return NULL;
#endif
}

/* The native adapter owns codec/audio objects; the guest owns only this
 * wire record. Closing/skipping releases both sides immediately. */
#ifdef HALO_MACOS
void __stdcall BinkClose(HBINK movie) { if (movie) { host_bink_close(movie->native_token); free(movie); } }
long __stdcall BinkDoFrame(HBINK movie)
{
	if (!movie || !host_bink_decode(movie->native_token)) host_abort("native Bink video decode failed");
	return 0;
}
void __stdcall BinkNextFrame(HBINK movie)
{
	if (!movie || !host_bink_next(movie->native_token, movie)) host_abort("native Bink frame state failed");
}
long __stdcall BinkWait(HBINK movie) { return movie ? host_bink_wait(movie->native_token) : 0; }
long __stdcall BinkCopyToBuffer(HBINK movie, void *destination, long pitch,
	unsigned long height, unsigned long x, unsigned long y, unsigned long flags)
{
	if (!movie || !host_bink_copy(movie->native_token, destination, pitch, height, x, y, flags))
		host_abort("native Bink destination span or format invalid");
	return 0;
}
void __stdcall BinkGetSummary(HBINK movie, void *summary)
{
	unsigned long *words = summary;
	memset(summary, 0, 31 * sizeof(unsigned long));
	if (movie) { words[0] = movie->Width; words[1] = movie->Height; words[8] = movie->Frames; words[9] = movie->FrameNum; }
}
void __stdcall BinkGetRealtime(HBINK movie, void *realtime, unsigned long frame_count)
{
	unsigned long *words = realtime;
	(void)frame_count;
	memset(realtime, 0, 14 * sizeof(unsigned long));
	if (movie) { words[0] = movie->FrameNum; words[3] = movie->FrameNum ? movie->FrameNum : 1; }
}
#else
/* never reached without an open movie */

void __stdcall BinkClose(HBINK bink) { (void)bink; }
long __stdcall BinkDoFrame(HBINK bink) { (void)bink; return 0; }
void __stdcall BinkNextFrame(HBINK bink) { (void)bink; }
long __stdcall BinkWait(HBINK bink) { (void)bink; return 0; }

long __stdcall BinkCopyToBuffer(HBINK bink, void *destination, long destination_pitch,
	unsigned long destination_height, unsigned long destination_x, unsigned long destination_y,
	unsigned long flags)
{
	(void)bink; (void)destination; (void)destination_pitch; (void)destination_height;
	(void)destination_x; (void)destination_y; (void)flags;
	return 0;
}

void __stdcall BinkGetSummary(HBINK bink, void *summary) { (void)bink; (void)summary; }
void __stdcall BinkGetRealtime(HBINK bink, void *realtime, unsigned long frame_count) { (void)bink; (void)realtime; (void)frame_count; }
#endif
