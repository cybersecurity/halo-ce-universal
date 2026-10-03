#include "host_graphics.h"
/*
HOST_SDL.C

SDL3 on behalf of the guest (guest/runtime/guest_sdl.c). SDL objects are
64-bit pointers, which the guest cannot hold; it gets small handles into the
table here instead.

The guest calls these on its own threads, whose stacks remain inside the
native guest arena (host_thread.c). SDL's audio thread is the exception: it has no guest stack,
so the audio callback is handed to a thread that has one.
*/

#include "host.h"

#include <SDL3/SDL.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>
#include "ios_host.h"

#define HANDLE_COUNT 256

enum handle_type
{
	_handle_free,
	_handle_window,
	_handle_context,
	_handle_gamepad,
	_handle_audio,
};

struct handle
{
	int type;
	void *object;
};

static struct handle handles[HANDLE_COUNT];
static pthread_mutex_t handle_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t handle_new(int type, void *object)
{
	uint32_t index;

	if (!object)
		return 0;
	pthread_mutex_lock(&handle_lock);
	/* an object that already has a handle keeps it */
	for (index = 1; index < HANDLE_COUNT; index++)
	{
		if (handles[index].type == type && handles[index].object == object)
		{
			pthread_mutex_unlock(&handle_lock);
			return index;
		}
	}
	for (index = 1; index < HANDLE_COUNT; index++)
	{
		if (handles[index].type == _handle_free)
		{
			handles[index].type = type;
			handles[index].object = object;
			pthread_mutex_unlock(&handle_lock);
			return index;
		}
	}
	pthread_mutex_unlock(&handle_lock);
	host_logf(HOST_LOG_ERROR, "out of SDL handles");
	return 0;
}

static void *handle_get(uint32_t handle, int type)
{
	void *object = NULL;

	if (handle == 0 || handle >= HANDLE_COUNT)
		return NULL;
	pthread_mutex_lock(&handle_lock);
	if (handles[handle].type == type)
		object = handles[handle].object;
	pthread_mutex_unlock(&handle_lock);
	return object;
}

/* ---------- general */

int host_sdl_init(uint32_t flags)
{
	return SDL_Init((SDL_InitFlags)flags);
}

int host_sdl_set_hint(const char *name, const char *value)
{
	return SDL_SetHint(name, value);
}

void host_sdl_get_error(char *buffer, uint32_t size)
{
	SDL_strlcpy(buffer, SDL_GetError(), size);
}

int64_t host_sdl_ticks(void)
{
	return (int64_t)SDL_GetTicks();
}

int64_t host_sdl_thread_id(void)
{
	return (int64_t)SDL_GetCurrentThreadID();
}

/* ---------- video */

uint32_t host_sdl_create_window(const char *title, int width, int height, int64_t flags)
{
	#if HALO_MACOS
    flags=(flags & ~SDL_WINDOW_FULLSCREEN) | SDL_WINDOW_RESIZABLE;
    width=1280;height=720;
#endif
    if(halo_graphics_metal())flags=(flags & ~SDL_WINDOW_OPENGL) | SDL_WINDOW_METAL;
	return handle_new(_handle_window, SDL_CreateWindow(title, width, height, (SDL_WindowFlags)flags));
}

void host_sdl_window_size_in_pixels(uint32_t window, int *width, int *height)
{
	SDL_Window *object = handle_get(window, _handle_window);

	*width = 0;
	*height = 0;
	if (object)
		SDL_GetWindowSizeInPixels(object, width, height);
}

int host_sdl_set_relative_mouse(uint32_t window, int enabled)
{
	SDL_Window *object = handle_get(window, _handle_window);

	return object ? SDL_SetWindowRelativeMouseMode(object, enabled != 0) : 0;
}

int host_sdl_gl_set_attribute(int attribute, int value)
{
	if(halo_graphics_metal())return 1;
	/* EAGL exposes ES 3.0; capabilities are queried from the created context. */
	if (attribute == SDL_GL_CONTEXT_MINOR_VERSION) value = 0;
	return SDL_GL_SetAttribute((SDL_GLAttr)attribute, value);
}

uint32_t host_sdl_gl_create_context(uint32_t window)
{
	SDL_Window *object = handle_get(window, _handle_window);
	SDL_GLContext context = object ? (halo_graphics_metal()?halo_graphics_create(object):SDL_GL_CreateContext(object)) : NULL;
	if (context) host_ios_touch_attach(object);
	return handle_new(_handle_context, context);
}

int host_sdl_gl_make_current(uint32_t window, uint32_t context)
{
	if(halo_graphics_metal())return halo_graphics_make_current();
	return SDL_GL_MakeCurrent(handle_get(window, _handle_window), handle_get(context, _handle_context));
}

int host_sdl_gl_set_swap_interval(int interval)
{
	return halo_graphics_metal()?halo_graphics_swap_interval(interval):SDL_GL_SetSwapInterval(interval);
}

#if HALO_MACOS
/* Capture this application's own framebuffer for unattended renderer checks. */
static void test_capture(SDL_Window *window) {
    static int frames;const char *path=SDL_getenv("HALO_MAC_TEST_FRAME");
    if(!path || ++frames!=60)return;
    int w,h;SDL_GetWindowSizeInPixels(window,&w,&h);
    unsigned char *pixels=SDL_malloc((size_t)w*h*4);if(!pixels)return;
    void (*readPixels)(int,int,int,int,unsigned,unsigned,void*)=halo_graphics_proc("glReadPixels");
    void (*getInteger)(unsigned,int*)=halo_graphics_proc("glGetIntegerv");
    void (*bindFramebuffer)(unsigned,unsigned)=halo_graphics_proc("glBindFramebuffer");
    int previous=0;getInteger(0x8CAA,&previous);bindFramebuffer(0x8CA8,0);
    readPixels(0,0,w,h,0x1908,0x1401,pixels);bindFramebuffer(0x8CA8,previous);
    SDL_Surface *surface=SDL_CreateSurfaceFrom(w,h,SDL_PIXELFORMAT_RGBA32,pixels,w*4);
    if(surface){SDL_FlipSurface(surface,SDL_FLIP_VERTICAL);SDL_SaveBMP(surface,path);SDL_DestroySurface(surface);}
    SDL_free(pixels);host_logf(HOST_LOG_INFO,"Saved test framebuffer: %s",path);
}
#endif
int host_sdl_gl_swap_window(uint32_t window)
{
	SDL_Window *object = handle_get(window, _handle_window);

#if HALO_MACOS
    if(object)test_capture(object);
#endif
	return object ? (halo_graphics_metal()?halo_graphics_swap():SDL_GL_SwapWindow(object)) : 0;
}

/* ---------- events */

int host_sdl_poll_event(void *event)
{
	SDL_Event host_event;

	#ifdef HALO_IOS_BROWSER_HOST
    { extern void host_ios_room_tick(void); host_ios_room_tick(); }
#endif
    if (!SDL_PollEvent(&host_event))
		return 0;
	/* the layouts agree except for the pointers of text, drop and user
	events, which the guest does not read */
	memcpy(event, &host_event, sizeof(host_event));
	return 1;
}

/* ---------- gamepads */

int host_sdl_get_gamepads(uint32_t *ids, int capacity)
{
	return host_ios_gamepads(ids, capacity);
}

uint32_t host_sdl_open_gamepad(uint32_t id)
{
	SDL_Gamepad *gamepad = SDL_OpenGamepad((SDL_JoystickID)id);

	if (gamepad)
		host_logf(HOST_LOG_INFO, "gamepad %u: %s (type %d, %04x:%04x)", (unsigned)id, SDL_GetGamepadName(gamepad),
			(int)SDL_GetGamepadType(gamepad), SDL_GetGamepadVendor(gamepad), SDL_GetGamepadProduct(gamepad));
	return handle_new(_handle_gamepad, gamepad);
}

uint32_t host_sdl_gamepad_from_id(uint32_t id)
{
	return handle_new(_handle_gamepad, SDL_GetGamepadFromID((SDL_JoystickID)id));
}

int host_sdl_gamepad_axis(uint32_t gamepad, int axis)
{
	SDL_Gamepad *object = handle_get(gamepad, _handle_gamepad);

	return object ? host_ios_gamepad_axis(object, axis) : 0;
}

int host_sdl_gamepad_button(uint32_t gamepad, int button)
{
	SDL_Gamepad *object = handle_get(gamepad, _handle_gamepad);

	return object ? host_ios_gamepad_button(object, button) : 0;
}

int host_sdl_gamepad_type(uint32_t gamepad)
{
	SDL_Gamepad *object = handle_get(gamepad, _handle_gamepad);

	return object ? host_ios_gamepad_type(object) : SDL_GAMEPAD_TYPE_UNKNOWN;
}

int host_sdl_rumble_gamepad(uint32_t gamepad, uint32_t low, uint32_t high, uint32_t milliseconds)
{
	SDL_Gamepad *object = handle_get(gamepad, _handle_gamepad);

	return object ? SDL_RumbleGamepad(object, (Uint16)low, (Uint16)high, milliseconds) : 0;
}

/* ---------- audio */

/* SDL holds its stream lock while calling audio_callback. The guest worker
must stage its PCM here: calling SDL_PutAudioStreamData from that worker would
wait for the lock held by the callback that is waiting for the worker. Only
the original SDL callback thread submits the staged samples to the stream. */
struct audio_binding
{
	uint32_t handle;
	uint32_t callback;
	uint32_t userdata;
	pthread_mutex_t lock;
	pthread_cond_t requested;
	pthread_cond_t done;
	int pending;
	int additional;
	int total;
	unsigned char *samples;
	int length;
	int capacity;
	SDL_AudioFormat format;
	int logged_output;
};

static __thread struct audio_binding *audio_worker_binding;

static void *audio_thread(void *context)
{
	struct audio_binding *binding = context;

	audio_worker_binding = binding;
	pthread_mutex_lock(&binding->lock);
	for (;;)
	{
		int additional, total;

		while (!binding->pending)
			pthread_cond_wait(&binding->requested, &binding->lock);
		additional = binding->additional;
		total = binding->total;
		pthread_mutex_unlock(&binding->lock);
		host_call_guest(binding->callback, binding->userdata, binding->handle, (uint32_t)additional, (uint32_t)total);
		pthread_mutex_lock(&binding->lock);
		binding->pending = 0;
		pthread_cond_signal(&binding->done);
	}
	return NULL;
}

static void SDLCALL audio_callback(void *userdata, SDL_AudioStream *stream, int additional, int total)
{
	struct audio_binding *binding = userdata;

	pthread_mutex_lock(&binding->lock);
	binding->length = 0;
	binding->additional = additional;
	binding->total = total;
	binding->pending = 1;
	pthread_cond_signal(&binding->requested);
	while (binding->pending)
		pthread_cond_wait(&binding->done, &binding->lock);
	if (binding->length && !SDL_PutAudioStreamData(stream, binding->samples, binding->length))
		host_logf(HOST_LOG_ERROR, "cannot submit audio: %s", SDL_GetError());
	if (!binding->logged_output && binding->format == SDL_AUDIO_F32)
	{
		const float *samples = (const float *)binding->samples;
		float peak = 0.0f;
		for (int i = 0; i < binding->length / (int)sizeof(float); ++i)
			peak = SDL_max(peak, SDL_fabsf(samples[i]));
		if (peak > 0.0001f)
		{
			host_logf(HOST_LOG_INFO, "audio output active: %d PCM bytes, peak %.4f", binding->length, peak);
			binding->logged_output = 1;
		}
	}
	pthread_mutex_unlock(&binding->lock);
}

uint32_t host_sdl_open_audio_stream(uint32_t device, const void *spec, uint32_t callback, uint32_t userdata)
{
	struct audio_binding *binding = SDL_calloc(1, sizeof(*binding));
	SDL_AudioStream *stream;

	if (!binding)
		return 0;
	binding->callback = callback;
	binding->userdata = userdata;
	binding->format = ((const SDL_AudioSpec *)spec)->format;
	pthread_mutex_init(&binding->lock, NULL);
	pthread_cond_init(&binding->requested, NULL);
	pthread_cond_init(&binding->done, NULL);
	stream = SDL_OpenAudioDeviceStream((SDL_AudioDeviceID)device, spec,
		callback ? audio_callback : NULL, binding);
	if (!stream)
	{
		pthread_cond_destroy(&binding->done);
		pthread_cond_destroy(&binding->requested);
		pthread_mutex_destroy(&binding->lock);
		SDL_free(binding);
		return 0;
	}
	/* the device starts paused, so no callback can run before this */
	binding->handle = handle_new(_handle_audio, stream);
	if (callback && host_native_thread_create(audio_thread, binding, 256 * 1024) != 0)
		host_fatal("cannot start the audio thread");
	return binding->handle;
}

int host_sdl_put_audio_stream_data(uint32_t stream, const void *data, int length)
{
	struct audio_binding *binding = audio_worker_binding;

	if (binding && binding->handle == stream)
	{
		/* The callback is asleep until this worker signals completion. Copy
		   now because the guest's mix buffer is on its stack and is reused. */
		if (length < 0 || (!data && length) || length > INT_MAX - binding->length)
			return SDL_SetError("invalid guest audio buffer");
		int needed = binding->length + length;
		if (needed > binding->capacity)
		{
			int capacity = SDL_max(needed, 65536);
			void *samples = SDL_realloc(binding->samples, (size_t)capacity);
			if (!samples)
				return SDL_OutOfMemory();
			binding->samples = samples;
			binding->capacity = capacity;
		}
		if (length)
			SDL_memcpy(binding->samples + binding->length, data, (size_t)length);
		binding->length = needed;
		return 1;
	}
	SDL_AudioStream *object = handle_get(stream, _handle_audio);

	return object ? SDL_PutAudioStreamData(object, data, length) : 0;
}

int host_sdl_resume_audio_stream_device(uint32_t stream)
{
	SDL_AudioStream *object = handle_get(stream, _handle_audio);

	return object ? SDL_ResumeAudioStreamDevice(object) : 0;
}

/* ---------- the clipboard (internet play's invite links) */

int host_sdl_set_clipboard_text(const char *text)
{
	return SDL_SetClipboardText(text) ? 1 : 0;
}

void host_sdl_get_clipboard_text(char *buffer, uint32_t size)
{
	char *text = SDL_GetClipboardText();

	SDL_strlcpy(buffer, text ? text : "", size);
	SDL_free(text);
}

/* UIKit renders to a view framebuffer, rather than framebuffer zero. */
uint32_t host_ios_default_framebuffer(void)
{
    if(halo_graphics_metal())return 0;
#if !HALO_MACOS
    for (unsigned i = 1; i < HANDLE_COUNT; i++)
        if (handles[i].type == _handle_window)
            return (uint32_t)SDL_GetNumberProperty(SDL_GetWindowProperties(handles[i].object),
                SDL_PROP_WINDOW_UIKIT_OPENGL_FRAMEBUFFER_NUMBER, 0);
#endif
    return 0;
}

int host_sdl_show_simple_message_box(unsigned int flags, const char *title, const char *message)
{
    return SDL_ShowSimpleMessageBox((SDL_MessageBoxFlags)flags, title, message, NULL) ? 1 : 0;
}

void host_sdl_window_size(unsigned int window,int *width,int *height) {
 SDL_Window *object=handle_get(window,_handle_window);*width=*height=0;
 if(object)SDL_GetWindowSize(object,width,height);
}
void host_sdl_warp_mouse(unsigned int window,float x,float y) {
 SDL_Window *object=handle_get(window,_handle_window);if(object)SDL_WarpMouseInWindow(object,x,y);
}
