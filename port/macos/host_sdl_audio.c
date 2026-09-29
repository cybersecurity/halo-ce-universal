#include "host_sdl_audio.h"

#include "guest_address.h"
#include "guest_callback.h"
#include "guest_call.h"
#include "host_services.h"

#include <SDL3/SDL.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define AUDIO_HANDLE_COUNT 256u
#define AUDIO_GUEST_STACK_SIZE (256u * 1024u)
#define AUDIO_LEAD_MS 40u
#define AUDIO_CHUNK_MS 10u
#define AUDIO_BUFFER_LIMIT (16u * 1024u * 1024u)

/* SDL_AudioSpec is 12 bytes in the 32-bit guest headers: 32-bit enum format,
 * channels, and sample rate. Read fields explicitly; never pass guest layout
 * memory as the arm64 host's SDL struct. */
struct guest_sdl_audio_spec
{
	uint32_t format;
	int32_t channels;
	int32_t frequency;
};
_Static_assert(sizeof(struct guest_sdl_audio_spec) == 12, "guest SDL_AudioSpec layout");

struct audio_binding
{
	uint32_t handle;
	uint32_t callback_va;
	uint32_t userdata_va;
	SDL_AudioStream *stream;
	pthread_t worker;
	pthread_mutex_t mutex;
	pthread_cond_t requested;
	pthread_cond_t completed;
	int sync_initialized;
	int worker_started;
	int worker_ready;
	int worker_error;
	int stopping;
	int pending;
	int producing;
	int prefilled;
	uint32_t lead_bytes;
	uint32_t chunk_bytes;
	_Atomic uint64_t underrun_requests;
	uint64_t max_queued_bytes;
	int additional;
	int total;
	unsigned char *buffer;
	size_t buffer_length;
	size_t buffer_capacity;
	SDL_AudioFormat format;
	int channels;
	int frequency;
	uint64_t callback_ns;
	uint64_t max_callback_ns;
	uint64_t request_ns;
	uint64_t worker_wake_ns;
	uint64_t max_worker_wake_ns;
	uint64_t guest_invoke_ns;
	uint64_t max_guest_invoke_ns;
	uint64_t late_callbacks;
	uint64_t short_callbacks;
	uint64_t sample_count;
	uint64_t nonfinite_samples;
	uint64_t clipped_samples;
	double sample_sum_squares;
	double sample_peak;
	uint64_t evidence_bytes;
	uint64_t evidence_nonzero_bytes;
	uint64_t evidence_callbacks;
	uint64_t evidence_report_ms;
};

static struct audio_binding *audio_handles[AUDIO_HANDLE_COUNT];
static pthread_mutex_t audio_handles_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t audio_shutdown_once = PTHREAD_ONCE_INIT;
static int audio_shutdown_registered;
static _Thread_local struct audio_binding *callback_binding;

static void register_audio_shutdown(void)
{
	audio_shutdown_registered = atexit(mac_host_sdl_audio_shutdown) == 0;
}

int mac_host_sdl_audio_enabled(void)
{
	const char *value = getenv("HALO_AUDIO_ENABLE");
	return value && strcmp(value, "1") == 0;
}

static uint32_t handle_add(struct audio_binding *binding)
{
	uint32_t index;
	pthread_mutex_lock(&audio_handles_mutex);
	for (index = 1; index < AUDIO_HANDLE_COUNT; ++index)
		if (!audio_handles[index])
		{
			audio_handles[index] = binding;
			binding->handle = index;
			pthread_mutex_unlock(&audio_handles_mutex);
			return index;
		}
	pthread_mutex_unlock(&audio_handles_mutex);
	return 0;
}

static struct audio_binding *handle_get(uint32_t handle)
{
	struct audio_binding *binding = NULL;
	if (handle == 0 || handle >= AUDIO_HANDLE_COUNT)
		return NULL;
	pthread_mutex_lock(&audio_handles_mutex);
	binding = audio_handles[handle];
	pthread_mutex_unlock(&audio_handles_mutex);
	return binding;
}

static void handle_remove(uint32_t handle)
{
	if (handle == 0 || handle >= AUDIO_HANDLE_COUNT)
		return;
	pthread_mutex_lock(&audio_handles_mutex);
	audio_handles[handle] = NULL;
	pthread_mutex_unlock(&audio_handles_mutex);
}

static int guest_callback_put(struct audio_binding *binding,
	uint32_t data_va, int length)
{
	const void *data;
	size_t needed, capacity;
	unsigned char *replacement;

	if (length < 0)
		return 0;
	if (length == 0)
		return 1;
	if (!(data = mac_guest_address_resolve(data_va, (size_t)length)) ||
		(size_t)length > AUDIO_BUFFER_LIMIT - binding->buffer_length)
		return 0;
	needed = binding->buffer_length + (size_t)length;
	if (needed > binding->buffer_capacity)
	{
		capacity = binding->buffer_capacity ? binding->buffer_capacity : 4096;
		while (capacity < needed && capacity < AUDIO_BUFFER_LIMIT / 2)
			capacity *= 2;
		if (capacity < needed)
			capacity = needed;
		replacement = SDL_realloc(binding->buffer, capacity);
		if (!replacement)
			return 0;
		binding->buffer = replacement;
		binding->buffer_capacity = capacity;
	}
	memcpy(binding->buffer + binding->buffer_length, data, (size_t)length);
	binding->buffer_length = needed;
	return 1;
}

static int audio_submit(struct audio_binding *binding)
{
	pthread_mutex_lock(&binding->mutex);
	uint64_t callback_ns = binding->callback_ns, max_callback_ns = binding->max_callback_ns;
	uint64_t underrun_requests = atomic_load_explicit(&binding->underrun_requests, memory_order_relaxed);
	pthread_mutex_unlock(&binding->mutex);
	if (binding->buffer_length)
	{
		if (!SDL_PutAudioStreamData(binding->stream, binding->buffer,
			(int)binding->buffer_length))
			return 0;
		else if (getenv("HALO_AUDIO_EVIDENCE"))
		{
			uint64_t now = SDL_GetTicks();
			binding->evidence_bytes += binding->buffer_length;
			++binding->evidence_callbacks;
			for (size_t i = 0; i < binding->buffer_length; ++i)
				binding->evidence_nonzero_bytes += binding->buffer[i] != 0;
			if (binding->format == SDL_AUDIO_F32LE)
				for (size_t i = 0; i + sizeof(float) <= binding->buffer_length; i += sizeof(float))
				{
					float sample;
					memcpy(&sample, binding->buffer + i, sizeof(sample));
					if (!isfinite(sample)) { ++binding->nonfinite_samples; continue; }
					double magnitude = fabs((double)sample);
					++binding->sample_count;
					binding->clipped_samples += magnitude > 1.0;
					binding->sample_sum_squares += (double)sample * sample;
					if (magnitude > binding->sample_peak) binding->sample_peak = magnitude;
				}
			if (!binding->evidence_report_ms || now - binding->evidence_report_ms >= 1000)
			{
				mac_host_logf(1, "[audio-evidence] stream=%u format=0x%04x callbacks=%llu submitted_bytes=%llu nonzero_bytes=%llu worker_error=%d",
					binding->handle, (unsigned)binding->format,
					(unsigned long long)binding->evidence_callbacks,
					(unsigned long long)binding->evidence_bytes,
					(unsigned long long)binding->evidence_nonzero_bytes, binding->worker_error);
				binding->evidence_report_ms = now;
				mac_host_logf(1, "[audio-quality] stream=%u rate=%d channels=%d samples=%llu nonfinite=%llu above_fullscale=%llu peak=%.6g rms=%.6g callback_ms=%.3f max_ms=%.3f late_callbacks=%llu short_callbacks=%llu requested=%d submitted=%zu worker_wake_ms=%.3f max_worker_wake_ms=%.3f guest_invoke_ms=%.3f max_guest_invoke_ms=%.3f underrun_requests=%llu max_queued_bytes=%llu lead_bytes=%u",
					binding->handle, binding->frequency, binding->channels,
					(unsigned long long)binding->sample_count, (unsigned long long)binding->nonfinite_samples,
					(unsigned long long)binding->clipped_samples, binding->sample_peak,
					binding->sample_count ? sqrt(binding->sample_sum_squares / binding->sample_count) : 0.0,
					callback_ns / 1e6, max_callback_ns / 1e6,
					(unsigned long long)binding->late_callbacks, (unsigned long long)binding->short_callbacks,
					binding->additional, binding->buffer_length,
					binding->worker_wake_ns / 1e6, binding->max_worker_wake_ns / 1e6,
					binding->guest_invoke_ns / 1e6, binding->max_guest_invoke_ns / 1e6,
					(unsigned long long)underrun_requests, (unsigned long long)binding->max_queued_bytes, binding->lead_bytes);
			}
		}
		binding->buffer_length = 0;
	}
	return 1;
}

static void *audio_guest_worker(void *opaque)
{
	struct audio_binding *binding = opaque;
	struct mac_guest_callback_context *guest_context = NULL;
	int error = mac_guest_callback_context_create_current(AUDIO_GUEST_STACK_SIZE,
		&guest_context);
	pthread_mutex_lock(&binding->mutex);
	binding->worker_error = error;
	binding->worker_ready = 1;
	pthread_cond_broadcast(&binding->completed);
	pthread_mutex_unlock(&binding->mutex);
	if (error != 0) return NULL;
	for (;;)
	{
		pthread_mutex_lock(&binding->mutex);
		while (!binding->producing && !binding->stopping)
			pthread_cond_wait(&binding->requested, &binding->mutex);
		if (binding->stopping)
		{
			pthread_mutex_unlock(&binding->mutex);
			break;
		}
		binding->pending = 0;
		pthread_mutex_unlock(&binding->mutex);
		/* Query/submit outside the binding mutex: SDL takes its stream lock,
		 * while the device callback briefly takes this mutex in reverse. */
		int queued = SDL_GetAudioStreamQueued(binding->stream);
		if (queued < 0) { error = EIO; break; }
		if ((uint32_t)queued > binding->lead_bytes) { error = EOVERFLOW; break; }
		if ((uint64_t)queued > binding->max_queued_bytes) binding->max_queued_bytes = queued;
		if ((uint32_t)queued < binding->lead_bytes)
		{
			uint32_t additional = binding->lead_bytes - (uint32_t)queued;
			if (additional > binding->chunk_bytes) additional = binding->chunk_bytes;
			/* The DirectSound callback ignores total and consumes precisely the
			 * requested input frames. Produce those frames early in 10ms chunks,
			 * retaining ordered cursor advancement with at most 40ms lead. */
			uint32_t arguments[4] = {binding->userdata_va, binding->handle,
				additional, binding->lead_bytes};
			uint64_t invoked_ns = SDL_GetTicksNS();
			pthread_mutex_lock(&binding->mutex);
			binding->additional = additional;
			binding->worker_wake_ns = binding->request_ns ? invoked_ns - binding->request_ns : 0;
			if (binding->worker_wake_ns > binding->max_worker_wake_ns)
				binding->max_worker_wake_ns = binding->worker_wake_ns;
			binding->request_ns = 0;
			pthread_mutex_unlock(&binding->mutex);
			callback_binding = binding;
			error = mac_guest_callback_invoke(guest_context, binding->callback_va, arguments, 4);
			callback_binding = NULL;
			binding->guest_invoke_ns = SDL_GetTicksNS() - invoked_ns;
			if (binding->guest_invoke_ns > binding->max_guest_invoke_ns)
				binding->max_guest_invoke_ns = binding->guest_invoke_ns;
			if (error) break;
			if (binding->buffer_length < additional) ++binding->short_callbacks;
			if (binding->guest_invoke_ns * (uint64_t)binding->frequency *
				SDL_AUDIO_BYTESIZE(binding->format) * binding->channels > (uint64_t)additional * 1000000000)
				++binding->late_callbacks;
			/* A callback must not turn the small lead into an unbounded queue. */
			if (!binding->buffer_length) { error = EIO; break; }
			if ((uint64_t)queued + binding->buffer_length > binding->lead_bytes) { error = EOVERFLOW; break; }
			if (!audio_submit(binding)) { error = EIO; break; }
			if (additional) continue;
		}
		pthread_mutex_lock(&binding->mutex);
		binding->prefilled = 1;
		pthread_cond_broadcast(&binding->completed);
		struct timespec deadline;
		clock_gettime(CLOCK_REALTIME, &deadline);
		deadline.tv_nsec += 5000000;
		if (deadline.tv_nsec >= 1000000000) { ++deadline.tv_sec; deadline.tv_nsec -= 1000000000; }
		if (!binding->pending && !binding->stopping)
			pthread_cond_timedwait(&binding->requested, &binding->mutex, &deadline);
		pthread_mutex_unlock(&binding->mutex);
	}
	pthread_mutex_lock(&binding->mutex);
	binding->worker_error = error;
	binding->stopping = 1;
	pthread_cond_broadcast(&binding->completed);
	pthread_mutex_unlock(&binding->mutex);
	mac_guest_callback_context_destroy_current(guest_context);
	return NULL;
}

static void SDLCALL audio_stream_callback(void *userdata, SDL_AudioStream *stream,
	int additional, int total)
{
	struct audio_binding *binding = userdata;
	uint64_t started_ns = SDL_GetTicksNS();
	(void)stream;
	(void)total;
	if (additional > 0)
		atomic_fetch_add_explicit(&binding->underrun_requests, 1, memory_order_relaxed);
	/* Never wait for guest code, the mixer lock, or the producer. SDL holds
	 * the stream lock on the shared physical output thread here. A skipped
	 * notification is harmless: the bounded producer also wakes every 5ms. */
	if (pthread_mutex_trylock(&binding->mutex) != 0) return;
	if (!binding->stopping && !binding->worker_error)
	{
		if (!binding->request_ns) binding->request_ns = started_ns;
		binding->pending = 1;
		pthread_cond_signal(&binding->requested);
	}
	binding->callback_ns = SDL_GetTicksNS() - started_ns;
	if (binding->callback_ns > binding->max_callback_ns)
		binding->max_callback_ns = binding->callback_ns;
	pthread_mutex_unlock(&binding->mutex);
}

uint32_t mac_host_sdl_open_audio_stream(uint32_t device, uint32_t spec_va,
	uint32_t callback_va, uint32_t userdata_va)
{
	struct guest_sdl_audio_spec guest_spec;
	SDL_AudioSpec host_spec;
	struct audio_binding *binding;
	SDL_AudioStream *stream;
	int error;

	if (!mac_host_sdl_audio_enabled())
		return 0;
	if (pthread_once(&audio_shutdown_once, register_audio_shutdown) != 0 ||
		!audio_shutdown_registered)
	{
		SDL_SetError("could not register SDL audio shutdown cleanup");
		return 0;
	}
	if (mac_guest_read(spec_va, &guest_spec, sizeof(guest_spec)) != 0 ||
		(guest_spec.channels <= 0 || guest_spec.channels > 32) || guest_spec.frequency <= 0)
	{
		SDL_SetError("invalid guest SDL_AudioSpec");
		return 0;
	}
	host_spec.format = (SDL_AudioFormat)guest_spec.format;
	host_spec.channels = guest_spec.channels;
	host_spec.freq = guest_spec.frequency;
	binding = SDL_calloc(1, sizeof(*binding));
	if (!binding)
		return 0;
	atomic_init(&binding->underrun_requests, 0);
	binding->callback_va = callback_va;
	binding->userdata_va = userdata_va;
	binding->format = host_spec.format;
	binding->channels = host_spec.channels;
	binding->frequency = host_spec.freq;
	uint64_t frame_bytes = SDL_AUDIO_BYTESIZE(host_spec.format) * (uint64_t)host_spec.channels;
	uint64_t lead = ((uint64_t)host_spec.freq * AUDIO_LEAD_MS + 999) / 1000 * frame_bytes;
	uint64_t chunk = ((uint64_t)host_spec.freq * AUDIO_CHUNK_MS + 999) / 1000 * frame_bytes;
	if (!frame_bytes || !lead || lead > AUDIO_BUFFER_LIMIT || chunk > INT32_MAX) goto fail_alloc;
	binding->lead_bytes = (uint32_t)lead;
	binding->chunk_bytes = (uint32_t)chunk;
	if (pthread_mutex_init(&binding->mutex, NULL) != 0)
		goto fail_alloc;
	if (pthread_cond_init(&binding->requested, NULL) != 0)
		goto fail_mutex;
	if (pthread_cond_init(&binding->completed, NULL) != 0)
		goto fail_requested;
	binding->sync_initialized = 1;
	stream = SDL_OpenAudioDeviceStream((SDL_AudioDeviceID)device, &host_spec,
		callback_va ? audio_stream_callback : NULL, binding);
	if (!stream)
		goto fail_sync;
	binding->stream = stream;
	if (!(binding->handle = handle_add(binding)))
		goto fail_stream;
	if (callback_va)
	{
		error = pthread_create(&binding->worker, NULL, audio_guest_worker, binding);
		if (error != 0)
		{
			SDL_SetError("could not start translated audio callback worker (%d)", error);
			goto fail_handle;
		}
		binding->worker_started = 1;
		pthread_mutex_lock(&binding->mutex);
		while (!binding->worker_ready)
			pthread_cond_wait(&binding->completed, &binding->mutex);
		error = binding->worker_error;
		pthread_mutex_unlock(&binding->mutex);
		if (error != 0)
		{
			SDL_SetError("could not attach translated audio callback thread (%d)", error);
			goto fail_handle;
		}
	}
	return binding->handle;

fail_handle:
	if (binding->worker_started)
	{
		pthread_mutex_lock(&binding->mutex);
		binding->stopping = 1;
		pthread_cond_broadcast(&binding->requested);
		pthread_mutex_unlock(&binding->mutex);
		(void)SDL_PauseAudioStreamDevice(binding->stream);
	}
	SDL_SetAudioStreamGetCallback(binding->stream, NULL, NULL);
	if (binding->worker_started)
		pthread_join(binding->worker, NULL);
	SDL_DestroyAudioStream(binding->stream);
	handle_remove(binding->handle);
	pthread_cond_destroy(&binding->completed);
	pthread_cond_destroy(&binding->requested);
	pthread_mutex_destroy(&binding->mutex);
	SDL_free(binding->buffer);
	SDL_free(binding);
	return 0;
fail_stream:
	SDL_DestroyAudioStream(stream);
fail_sync:
	pthread_cond_destroy(&binding->completed);
fail_requested:
	pthread_cond_destroy(&binding->requested);
fail_mutex:
	pthread_mutex_destroy(&binding->mutex);
fail_alloc:
	SDL_free(binding);
	return 0;
}

int mac_host_sdl_put_audio_stream_data(uint32_t stream_handle,
	uint32_t data_va, int length)
{
	struct audio_binding *binding = handle_get(stream_handle);
	const void *data;
	if (!binding || length < 0)
		return 0;
	if (callback_binding == binding)
		return guest_callback_put(binding, data_va, length);
	if (!length)
		return 1;
	if (!(data = mac_guest_address_resolve(data_va, (size_t)length)))
		return 0;
	return SDL_PutAudioStreamData(binding->stream, data, length) ? 1 : 0;
}

int mac_host_sdl_resume_audio_stream_device(uint32_t stream_handle)
{
	struct audio_binding *binding = handle_get(stream_handle);
	if (!binding) return 0;
	if (binding->worker_started)
	{
		pthread_mutex_lock(&binding->mutex);
		binding->producing = 1;
		pthread_cond_signal(&binding->requested);
		while (!binding->prefilled && !binding->worker_error && !binding->stopping)
			pthread_cond_wait(&binding->completed, &binding->mutex);
		int ready = binding->prefilled && !binding->worker_error;
		pthread_mutex_unlock(&binding->mutex);
		if (!ready) return 0;
	}
	return SDL_ResumeAudioStreamDevice(binding->stream) ? 1 : 0;
}

void mac_host_sdl_audio_shutdown(void)
{
	for (uint32_t handle = 1; handle < AUDIO_HANDLE_COUNT; ++handle)
	{
		struct audio_binding *binding = handle_get(handle);
		if (!binding)
			continue;
		if (binding->worker_started)
		{
			pthread_mutex_lock(&binding->mutex);
			binding->stopping = 1;
			pthread_cond_broadcast(&binding->requested);
			pthread_mutex_unlock(&binding->mutex);
			(void)SDL_PauseAudioStreamDevice(binding->stream);
		}
		SDL_SetAudioStreamGetCallback(binding->stream, NULL, NULL);
		if (binding->worker_started)
			pthread_join(binding->worker, NULL);
		SDL_DestroyAudioStream(binding->stream);
		handle_remove(handle);
		if (binding->sync_initialized)
		{
			pthread_cond_destroy(&binding->completed);
			pthread_cond_destroy(&binding->requested);
			pthread_mutex_destroy(&binding->mutex);
		}
		SDL_free(binding->buffer);
		SDL_free(binding);
	}
}
