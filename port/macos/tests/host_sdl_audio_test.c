#include "guest_address.h"
#include "guest_allocator.h"
#include "guest_callback.h"
#include "guest_call.h"
#include "host_services.h"
#include "host_sdl_audio.h"

#include <SDL3/SDL.h>
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TEST_GUEST_BASE UINT32_C(0x12000000)
#define TEST_GUEST_SIZE (1024u * 1024u)
#define TEST_ATTACH_VA UINT32_C(0x00401000)
#define TEST_CALLBACK_VA UINT32_C(0x00402000)
#define TEST_TP UINT32_C(0x12001000)
#define TEST_SPEC_VA UINT32_C(0x12002000)
#define TEST_PCM_VA UINT32_C(0x12003000)
#define TEST_PCM_BYTES 4096u

struct guest_sdl_audio_spec
{
	uint32_t format;
	int32_t channels;
	int32_t frequency;
};

static unsigned char guest_memory[TEST_GUEST_SIZE];
static uint32_t allocation_cursor = TEST_GUEST_BASE + 0x10000;
static pthread_mutex_t callback_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t callback_cond = PTHREAD_COND_INITIALIZER;
static unsigned callback_count;
static unsigned successful_puts;
static uint32_t expected_userdata;
static uint32_t observed_stream;
static uint32_t observed_additional;
static uint32_t observed_total;
static unsigned movie_callbacks;
static void SDLCALL movie_callback(void *userdata, SDL_AudioStream *stream, int additional, int total)
{
	(void)userdata; (void)total;
	unsigned char pcm[8192] = {0};
	assert(additional >= 0);
	while (additional > 0) {
		int bytes = additional < (int)sizeof(pcm) ? additional : (int)sizeof(pcm);
		assert(SDL_PutAudioStreamData(stream, pcm, bytes));
		additional -= bytes;
	}
	pthread_mutex_lock(&callback_mutex);
	++movie_callbacks;
	pthread_mutex_unlock(&callback_mutex);
}

static int test_allocate(void *context, size_t size, size_t alignment,
	uint32_t *guest_va_out)
{
	uint64_t aligned = ((uint64_t)allocation_cursor + alignment - 1) & ~(uint64_t)(alignment - 1);
	(void)context;
	if (!guest_va_out || aligned + size > TEST_GUEST_BASE + TEST_GUEST_SIZE)
		return -1;
	*guest_va_out = (uint32_t)aligned;
	allocation_cursor = (uint32_t)(aligned + size);
	return 0;
}

static void test_release(void *context, uint32_t guest_va, size_t size)
{
	(void)context;
	(void)guest_va;
	(void)size;
}

static void test_dispatch(void *context, uint32_t guest_function_va)
{
	uint32_t esp, frame[5];
	(void)context;
	if (guest_function_va == TEST_ATTACH_VA)
	{
		mac_host_set_guest_tp(TEST_TP);
		return;
	}
	assert(guest_function_va == TEST_CALLBACK_VA);
	/* The callback dispatcher has installed the guest ESP into its runtime;
	 * read it from the test worker's scratch slot populated by initialize. */
	extern _Thread_local uint32_t test_guest_esp;
	esp = test_guest_esp;
	assert(mac_guest_read(esp, frame, sizeof(frame)) == 0);
	assert(frame[1] == expected_userdata);
	pthread_mutex_lock(&callback_mutex);
	observed_stream = frame[2];
	observed_additional = frame[3];
	observed_total = frame[4];
	int delay_callback = callback_count == 4;
	++callback_count;
	pthread_cond_broadcast(&callback_cond);
	pthread_mutex_unlock(&callback_mutex);
	if (delay_callback)
		SDL_Delay(100);
	assert(mac_host_sdl_put_audio_stream_data(observed_stream, TEST_PCM_VA,
		(int)observed_additional) == 1);
	pthread_mutex_lock(&callback_mutex);
	++successful_puts;
	pthread_cond_broadcast(&callback_cond);
	pthread_mutex_unlock(&callback_mutex);
}

_Thread_local uint32_t test_guest_esp;

static void test_initialize_with_esp(void *context, uint32_t guest_thread_va, uint32_t guest_esp)
{
	(void)context;
	(void)guest_thread_va;
	test_guest_esp = guest_esp;
}

static int wait_for_callbacks(unsigned minimum)
{
	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += 3;
	pthread_mutex_lock(&callback_mutex);
	while (callback_count < minimum)
	{
		int error = pthread_cond_timedwait(&callback_cond, &callback_mutex, &deadline);
		if (error != 0)
			break;
	}
	int ready = callback_count >= minimum;
	pthread_mutex_unlock(&callback_mutex);
	return ready;
}

int main(void)
{
	struct mac_guest_allocator allocator = { NULL, test_allocate, test_release };
	struct mac_guest_thread_runtime runtime = { NULL, test_initialize_with_esp, test_dispatch };
	struct guest_sdl_audio_spec spec = { SDL_AUDIO_S16LE, 2, 22050 };
	const char *driver = getenv("HALO_AUDIO_TEST_DRIVER");
	uint32_t stream;

	assert(mac_guest_address_register(TEST_GUEST_BASE, guest_memory, sizeof(guest_memory)) == 0);
	assert(mac_guest_allocator_install(&allocator) == 0);
	assert(mac_guest_callback_runtime_install(&runtime, TEST_ATTACH_VA) == 0);
	assert(mac_guest_write(TEST_SPEC_VA, &spec, sizeof(spec)) == 0);
	memset(guest_memory + (TEST_PCM_VA - TEST_GUEST_BASE), 0, TEST_PCM_BYTES);
	expected_userdata = 0x55667788;
	assert(setenv("HALO_AUDIO_ENABLE", "1", 1) == 0);
	if (!driver || !*driver)
		driver = "dummy";
	assert(SDL_SetHint(SDL_HINT_AUDIO_DRIVER, driver));
	assert(SDL_Init(SDL_INIT_AUDIO));
	assert(SDL_GetCurrentAudioDriver() && strcmp(SDL_GetCurrentAudioDriver(), driver) == 0);

	stream = mac_host_sdl_open_audio_stream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
		TEST_SPEC_VA, TEST_CALLBACK_VA, expected_userdata);
	assert(stream != 0);
	SDL_AudioSpec movie_spec = {SDL_AUDIO_F32, 2, 48000};
	SDL_AudioStream *movie = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
		&movie_spec, movie_callback, NULL);
	assert(movie && SDL_ResumeAudioStreamDevice(movie));
	assert(mac_host_sdl_resume_audio_stream_device(stream) == 1);
	assert(wait_for_callbacks(5));
	pthread_mutex_lock(&callback_mutex);
	unsigned before_movie = movie_callbacks;
	pthread_mutex_unlock(&callback_mutex);
	SDL_Delay(70); /* Guest mixing remains stalled for 100ms. */
	pthread_mutex_lock(&callback_mutex);
	unsigned during_stall = movie_callbacks - before_movie;
	pthread_mutex_unlock(&callback_mutex);
	assert(during_stall >= 2); /* Shared physical output keeps servicing movie. */
	pthread_mutex_lock(&callback_mutex);
	assert(observed_stream == stream);
	assert(observed_additional > 0 && observed_total >= observed_additional);
	pthread_mutex_unlock(&callback_mutex);
	pthread_mutex_lock(&callback_mutex);
	unsigned before_shutdown = movie_callbacks;
	pthread_mutex_unlock(&callback_mutex);
	mac_host_sdl_audio_shutdown();
	SDL_Delay(70);
	pthread_mutex_lock(&callback_mutex);
	assert(movie_callbacks >= before_shutdown + 2);
	unsigned before_pause = movie_callbacks;
	pthread_mutex_unlock(&callback_mutex);
	assert(SDL_PauseAudioStreamDevice(movie));
	SDL_Delay(50);
	pthread_mutex_lock(&callback_mutex);
	assert(movie_callbacks <= before_pause + 1);
	pthread_mutex_unlock(&callback_mutex);
	assert(SDL_ResumeAudioStreamDevice(movie));
	SDL_Delay(70);
	pthread_mutex_lock(&callback_mutex);
	assert(movie_callbacks >= before_pause + 2);
	pthread_mutex_unlock(&callback_mutex);
	assert(successful_puts >= 1);
	SDL_DestroyAudioStream(movie);
	SDL_Quit();
	mac_guest_address_reset();
	puts("async audio: 100ms guest stall preserves movie callbacks; shutdown/pause/resume pass");
	return 0;
}
