/*
HOST_SDL.C

SDL3 on behalf of the guest (port/android/guest/runtime/guest_sdl.c and
port/macos/guest/runtime/guest_sdl_desktop.c). SDL objects are 64-bit
pointers, which the guest cannot hold; it gets small handles into the table
here instead.

Adapted from the Android port's host_sdl.c, with the desktop's window
management, renderer (the first start's extraction progress), message boxes
and file dialog added. SDL's audio thread has no guest stack, so the audio
callback is handed to a thread that has one.
*/

#include "host.h"

#include <SDL3/SDL.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HANDLE_COUNT 256

enum handle_type
{
	_handle_free,
	_handle_window,
	_handle_context,
	_handle_gamepad,
	_handle_audio,
	_handle_renderer,
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
	return SDL_GL_SetAttribute((SDL_GLAttr)attribute, value);
}

uint32_t host_sdl_gl_create_context(uint32_t window)
{
	SDL_Window *object = handle_get(window, _handle_window);

	return object ? handle_new(_handle_context, SDL_GL_CreateContext(object)) : 0;
}

int host_sdl_gl_make_current(uint32_t window, uint32_t context)
{
	return SDL_GL_MakeCurrent(handle_get(window, _handle_window), handle_get(context, _handle_context));
}

int host_sdl_gl_set_swap_interval(int interval)
{
	return SDL_GL_SetSwapInterval(interval);
}

/* host_metal_rt.m's waits */
extern uint64_t host_rt_finish_ns, host_rt_trace_ns, host_rt_traces;

/* HALO_FPS=1 (or HALO_PROFILE=1): the frames a second and their times,
logged every 5 seconds */
static void frame_statistics(void)
{
	static int enabled = -1;
	static uint64_t window_start, last, frames, slowest;
	uint64_t now = SDL_GetTicksNS();

	if (enabled < 0)
		enabled = getenv("HALO_FPS") || getenv("HALO_PROFILE");
	if (!enabled)
		return;
	if (!window_start)
	{
		window_start = last = now;
		return;
	}
	frames++;
	if (now - last > slowest)
		slowest = now - last;
	last = now;
	if (now - window_start >= 5000000000ull)
	{
		double seconds = (double)(now - window_start) / 1e9;

		host_logf(HOST_LOG_INFO, "fps %.1f (frame %.2f ms average, %.2f ms slowest; per frame %.2f ms waiting for GL, "
			"%.2f ms for Metal's rays, %llu traces)", (double)frames / seconds,
			seconds * 1000.0 / (double)frames, (double)slowest / 1e6,
			(double)host_rt_finish_ns / 1e6 / (double)frames, (double)host_rt_trace_ns / 1e6 / (double)frames,
			(unsigned long long)host_rt_traces);
		host_rt_finish_ns = host_rt_trace_ns = host_rt_traces = 0;
		window_start = now;
		frames = 0;
		slowest = 0;
	}
}

int host_sdl_gl_swap_window(uint32_t window)
{
	SDL_Window *object = handle_get(window, _handle_window);

	frame_statistics();
	return object ? SDL_GL_SwapWindow(object) : 0;
}

/* ---------- events */

/* a halo:// link macOS opened the game with (SDL passes the open-URL event
as a drop): written where the game looks for one (p2p.c poll_invite_file);
1 if the event was one */
static int invite_link(const SDL_Event *event)
{
	char path[1200];
	FILE *file;

	if (event->type != SDL_EVENT_DROP_FILE || !event->drop.data || strncmp(event->drop.data, "halo://", 7))
		return 0;
	snprintf(path, sizeof(path), "%s/join_link.txt", host_data_root);
	file = fopen(path, "wb");
	if (file)
	{
		fputs(event->drop.data, file);
		fclose(file);
		host_logf(HOST_LOG_INFO, "opened with an invite link");
	}
	return 1;
}

int host_sdl_poll_event(void *event)
{
	SDL_Event host_event;

	do
	{
		if (!SDL_PollEvent(&host_event))
			return 0;
	}
	while (invite_link(&host_event));
	/* the layouts agree except for the pointers of text, drop and user
	events, which the guest does not read */
	memcpy(event, &host_event, sizeof(host_event));
	return 1;
}

/* ---------- gamepads */

int host_sdl_get_gamepads(uint32_t *ids, int capacity)
{
	int count = 0, index;
	SDL_JoystickID *list = SDL_GetGamepads(&count);

	if (!list)
		return 0;
	if (count > capacity)
		count = capacity;
	for (index = 0; index < count; index++)
		ids[index] = list[index];
	SDL_free(list);
	return count;
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

	return object ? SDL_GetGamepadAxis(object, (SDL_GamepadAxis)axis) : 0;
}

int host_sdl_gamepad_button(uint32_t gamepad, int button)
{
	SDL_Gamepad *object = handle_get(gamepad, _handle_gamepad);

	return object ? SDL_GetGamepadButton(object, (SDL_GamepadButton)button) : 0;
}

int host_sdl_gamepad_type(uint32_t gamepad)
{
	SDL_Gamepad *object = handle_get(gamepad, _handle_gamepad);

	return object ? SDL_GetGamepadType(object) : SDL_GAMEPAD_TYPE_UNKNOWN;
}

int host_sdl_rumble_gamepad(uint32_t gamepad, uint32_t low, uint32_t high, uint32_t milliseconds)
{
	SDL_Gamepad *object = handle_get(gamepad, _handle_gamepad);

	return object ? SDL_RumbleGamepad(object, (Uint16)low, (Uint16)high, milliseconds) : 0;
}

/* ---------- audio */

/* SDL calls audio_callback on its own audio thread, which cannot run guest
code; it passes each request to the stream's thread (audio_thread), which
can, and waits for it to be done. SDL holds the stream's lock throughout, so
the audio the guest puts into the stream meanwhile (from audio_thread) is
kept in the binding's buffer instead, and put in by audio_callback once the
guest is done: audio_thread putting it in itself would wait for the lock
forever */
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
	unsigned char *buffer;
	int buffer_length;
	int buffer_size;
};

/* the binding whose callback this thread is running, if any */
static __thread struct audio_binding *calling_back;

static void *audio_thread(void *context)
{
	struct audio_binding *binding = context;

	pthread_mutex_lock(&binding->lock);
	for (;;)
	{
		int additional, total;

		while (!binding->pending)
			pthread_cond_wait(&binding->requested, &binding->lock);
		additional = binding->additional;
		total = binding->total;
		pthread_mutex_unlock(&binding->lock);
		calling_back = binding;
		host_call_guest(binding->callback, binding->userdata, binding->handle, (uint32_t)additional, (uint32_t)total);
		calling_back = NULL;
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
	binding->additional = additional;
	binding->total = total;
	binding->pending = 1;
	pthread_cond_signal(&binding->requested);
	while (binding->pending)
		pthread_cond_wait(&binding->done, &binding->lock);
	pthread_mutex_unlock(&binding->lock);
	if (binding->buffer_length)
	{
		SDL_PutAudioStreamData(stream, binding->buffer, binding->buffer_length);
		binding->buffer_length = 0;
	}
}

/* audio the guest puts into its stream during the stream's callback, for
audio_callback to put in; 1 on success */
static int audio_keep(struct audio_binding *binding, const void *data, int length)
{
	if (length < 0)
		return 0;
	if (binding->buffer_length + length > binding->buffer_size)
	{
		int size = (binding->buffer_length + length) * 2;
		unsigned char *buffer = SDL_realloc(binding->buffer, (size_t)size);

		if (!buffer)
			return 0;
		binding->buffer = buffer;
		binding->buffer_size = size;
	}
	memcpy(binding->buffer + binding->buffer_length, data, (size_t)length);
	binding->buffer_length += length;
	return 1;
}

uint32_t host_sdl_open_audio_stream(uint32_t device, const void *spec, uint32_t callback, uint32_t userdata)
{
	struct audio_binding *binding = SDL_calloc(1, sizeof(*binding));
	SDL_AudioStream *stream;

	binding->callback = callback;
	binding->userdata = userdata;
	pthread_mutex_init(&binding->lock, NULL);
	pthread_cond_init(&binding->requested, NULL);
	pthread_cond_init(&binding->done, NULL);
	stream = SDL_OpenAudioDeviceStream((SDL_AudioDeviceID)device, spec,
		callback ? audio_callback : NULL, binding);
	if (!stream)
	{
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
	SDL_AudioStream *object = handle_get(stream, _handle_audio);

	if (!object)
		return 0;
	if (calling_back && calling_back->handle == stream)
		return audio_keep(calling_back, data, length);
	return SDL_PutAudioStreamData(object, data, length);
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

int host_sdl_show_toast(const char *message, int duration, int gravity, int x, int y)
{
	(void)duration;
	(void)gravity;
	(void)x;
	(void)y;
	host_logf(HOST_LOG_INFO, "%s", message);
	return 1;
}

/* ---------- a message for the player (a host of another network version) */

int host_sdl_show_simple_message_box(uint32_t flags, const char *title, const char *message)
{
	return SDL_ShowSimpleMessageBox((SDL_MessageBoxFlags)flags, title, message, NULL) ? 1 : 0;
}

/* ---------- the desktop's windows (sdl_platform.c) */

void host_sdl_destroy_window(uint32_t window)
{
	SDL_Window *object = handle_get(window, _handle_window);

	if (!object)
		return;
	pthread_mutex_lock(&handle_lock);
	handles[window].type = _handle_free;
	handles[window].object = NULL;
	pthread_mutex_unlock(&handle_lock);
	SDL_DestroyWindow(object);
}

int64_t host_sdl_get_window_flags(uint32_t window)
{
	SDL_Window *object = handle_get(window, _handle_window);

	return object ? (int64_t)SDL_GetWindowFlags(object) : 0;
}

int host_sdl_set_window_fullscreen(uint32_t window, int fullscreen)
{
	SDL_Window *object = handle_get(window, _handle_window);

	return object ? SDL_SetWindowFullscreen(object, fullscreen != 0) : 0;
}

int host_sdl_get_window_size(uint32_t window, int *width, int *height)
{
	SDL_Window *object = handle_get(window, _handle_window);

	*width = 0;
	*height = 0;
	return object ? SDL_GetWindowSize(object, width, height) : 0;
}

void host_sdl_warp_mouse_in_window(uint32_t window, float x, float y)
{
	SDL_WarpMouseInWindow(handle_get(window, _handle_window), x, y);
}

uint32_t host_sdl_get_primary_display(void)
{
	return SDL_GetPrimaryDisplay();
}

uint32_t host_sdl_get_display_for_window(uint32_t window)
{
	SDL_Window *object = handle_get(window, _handle_window);

	return object ? SDL_GetDisplayForWindow(object) : 0;
}

/* the display's desktop mode, as fixed-width fields: format, w, h,
refresh numerator, refresh denominator; the density and rate as floats */
int host_sdl_get_desktop_display_mode(uint32_t display, int32_t *fields, float *reals)
{
	const SDL_DisplayMode *mode = SDL_GetDesktopDisplayMode((SDL_DisplayID)display);

	if (!mode)
		return 0;
	fields[0] = (int32_t)mode->format;
	fields[1] = mode->w;
	fields[2] = mode->h;
	fields[3] = mode->refresh_rate_numerator;
	fields[4] = mode->refresh_rate_denominator;
	reals[0] = mode->pixel_density;
	reals[1] = mode->refresh_rate;
	return 1;
}

int host_sdl_wait_event_timeout(void *event, int milliseconds)
{
	SDL_Event host_event;

	if (!SDL_WaitEventTimeout(event ? &host_event : NULL, milliseconds))
		return 0;
	if (event && invite_link(&host_event))
		return 0;
	if (event)
		memcpy(event, &host_event, sizeof(host_event));
	return 1;
}

/* ---------- the renderer (the first start's extraction progress) */

uint32_t host_sdl_create_renderer(uint32_t window)
{
	SDL_Window *object = handle_get(window, _handle_window);

	return object ? handle_new(_handle_renderer, SDL_CreateRenderer(object, NULL)) : 0;
}

void host_sdl_destroy_renderer(uint32_t renderer)
{
	SDL_Renderer *object = handle_get(renderer, _handle_renderer);

	if (!object)
		return;
	pthread_mutex_lock(&handle_lock);
	handles[renderer].type = _handle_free;
	handles[renderer].object = NULL;
	pthread_mutex_unlock(&handle_lock);
	SDL_DestroyRenderer(object);
}

int host_sdl_set_render_vsync(uint32_t renderer, int vsync)
{
	SDL_Renderer *object = handle_get(renderer, _handle_renderer);

	return object ? SDL_SetRenderVSync(object, vsync) : 0;
}

int host_sdl_set_render_draw_color(uint32_t renderer, uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
	SDL_Renderer *object = handle_get(renderer, _handle_renderer);

	return object ? SDL_SetRenderDrawColor(object, (Uint8)r, (Uint8)g, (Uint8)b, (Uint8)a) : 0;
}

int host_sdl_set_render_scale(uint32_t renderer, float x, float y)
{
	SDL_Renderer *object = handle_get(renderer, _handle_renderer);

	return object ? SDL_SetRenderScale(object, x, y) : 0;
}

int host_sdl_render_clear(uint32_t renderer)
{
	SDL_Renderer *object = handle_get(renderer, _handle_renderer);

	return object ? SDL_RenderClear(object) : 0;
}

int host_sdl_render_fill_rect(uint32_t renderer, const float *rectangle)
{
	SDL_Renderer *object = handle_get(renderer, _handle_renderer);
	SDL_FRect copy;

	if (!object)
		return 0;
	if (!rectangle)
		return SDL_RenderFillRect(object, NULL);
	copy.x = rectangle[0];
	copy.y = rectangle[1];
	copy.w = rectangle[2];
	copy.h = rectangle[3];
	return SDL_RenderFillRect(object, &copy);
}

int host_sdl_render_debug_text(uint32_t renderer, float x, float y, const char *text)
{
	SDL_Renderer *object = handle_get(renderer, _handle_renderer);

	return object ? SDL_RenderDebugText(object, x, y, text) : 0;
}

int host_sdl_render_present(uint32_t renderer)
{
	SDL_Renderer *object = handle_get(renderer, _handle_renderer);

	return object ? SDL_RenderPresent(object) : 0;
}

/* ---------- message boxes with buttons

The guest's SDL_MessageBoxData holds 32-bit pointers; it passes the parts
instead: the button texts one after another, each NUL-terminated. */

int host_sdl_show_message_box(uint32_t flags, const char *title, const char *message, int count,
	const int32_t *button_flags, const int32_t *button_ids, const char *texts, int32_t *chosen)
{
	SDL_MessageBoxButtonData buttons[8];
	SDL_MessageBoxData data;
	int index, answer = -1, result;

	if (count > 8)
		count = 8;
	for (index = 0; index < count; index++)
	{
		buttons[index].flags = (SDL_MessageBoxButtonFlags)button_flags[index];
		buttons[index].buttonID = button_ids[index];
		buttons[index].text = texts;
		texts += strlen(texts) + 1;
	}
	memset(&data, 0, sizeof(data));
	data.flags = (SDL_MessageBoxFlags)flags;
	data.title = title;
	data.message = message;
	data.numbuttons = count;
	data.buttons = buttons;
	result = SDL_ShowMessageBox(&data, &answer);
	*chosen = answer;
	return result ? 1 : 0;
}

/* ---------- the open file dialog (the first start's disc image)

The guest's callback cannot be called from SDL's callback, whose thread may
have no guest stack: the chosen path is kept here, and the guest asks for it
(host_sdl_file_dialog_result) while it waits. */

static pthread_mutex_t dialog_lock = PTHREAD_MUTEX_INITIALIZER;
/* 0 while open, 1 when a file was chosen, -1 when cancelled */
static int dialog_state = -1;
static char dialog_path[1024];

static void SDLCALL dialog_done(void *userdata, const char *const *files, int filter)
{
	(void)userdata;
	(void)filter;
	pthread_mutex_lock(&dialog_lock);
	if (files && files[0])
	{
		SDL_strlcpy(dialog_path, files[0], sizeof(dialog_path));
		dialog_state = 1;
	}
	else
	{
		dialog_state = -1;
	}
	pthread_mutex_unlock(&dialog_lock);
}

/* filters as "name\0pattern\0name\0pattern\0..." */
void host_sdl_show_open_file_dialog(const char *filters, int count)
{
	static SDL_DialogFileFilter host_filters[8];
	static char storage[1024];
	char *copy = storage;
	int index;

	if (count > 8)
		count = 8;
	for (index = 0; index < count; index++)
	{
		size_t length = strlen(filters) + 1;

		if (copy + length > storage + sizeof(storage))
		{
			count = index;
			break;
		}
		memcpy(copy, filters, length);
		host_filters[index].name = copy;
		copy += length;
		filters += length;
		length = strlen(filters) + 1;
		if (copy + length > storage + sizeof(storage))
		{
			count = index;
			break;
		}
		memcpy(copy, filters, length);
		host_filters[index].pattern = copy;
		copy += length;
		filters += length;
	}
	pthread_mutex_lock(&dialog_lock);
	dialog_state = 0;
	dialog_path[0] = 0;
	pthread_mutex_unlock(&dialog_lock);
	SDL_ShowOpenFileDialog(dialog_done, NULL, NULL, host_filters, count, NULL, false);
}

int host_sdl_file_dialog_result(char *buffer, uint32_t size)
{
	int state;

	pthread_mutex_lock(&dialog_lock);
	state = dialog_state;
	if (state == 1)
		SDL_strlcpy(buffer, dialog_path, size);
	pthread_mutex_unlock(&dialog_lock);
	return state;
}
