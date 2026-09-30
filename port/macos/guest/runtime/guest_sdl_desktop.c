/*
GUEST_SDL_DESKTOP.C

The SDL3 functions the desktop platform layer calls (sdl_platform.c,
port_config.c, updater.c) beyond the Android port's set
(port/android/guest/runtime/guest_sdl.c), for the macOS port's guest.

What needs no window system is done here in the guest: atomics, mutexes and
threads (musl's), and files (stdio). The rest goes to the host
(port/macos/host/host_sdl.c) with windows and renderers as small handles,
and with structures that hold pointers passed as their parts.
*/

/* before SDL: musl's alloca.h must come first */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <SDL3/SDL.h>

#include "guest_host.h"
#include "guest_host_desktop.h"

/* ---------- atomics and mutexes */

int SDL_SetAtomicInt(SDL_AtomicInt *a, int v)
{
	return __atomic_exchange_n(&a->value, v, __ATOMIC_SEQ_CST);
}

int SDL_GetAtomicInt(SDL_AtomicInt *a)
{
	return __atomic_load_n(&a->value, __ATOMIC_SEQ_CST);
}

SDL_Mutex *SDL_CreateMutex(void)
{
	pthread_mutex_t *mutex = malloc(sizeof(*mutex));
	pthread_mutexattr_t attributes;

	if (!mutex)
		return NULL;
	/* SDL's mutexes are recursive */
	pthread_mutexattr_init(&attributes);
	pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(mutex, &attributes);
	pthread_mutexattr_destroy(&attributes);
	return (SDL_Mutex *)mutex;
}

void SDL_LockMutex(SDL_Mutex *mutex)
{
	if (mutex)
		pthread_mutex_lock((pthread_mutex_t *)mutex);
}

void SDL_UnlockMutex(SDL_Mutex *mutex)
{
	if (mutex)
		pthread_mutex_unlock((pthread_mutex_t *)mutex);
}

void SDL_DestroyMutex(SDL_Mutex *mutex)
{
	if (mutex)
	{
		pthread_mutex_destroy((pthread_mutex_t *)mutex);
		free(mutex);
	}
}

/* ---------- threads */

struct guest_sdl_thread
{
	pthread_t thread;
	SDL_ThreadFunction function;
	void *data;
	int status;
};

static void *sdl_thread_main(void *context)
{
	struct guest_sdl_thread *thread = context;

	thread->status = thread->function(thread->data);
	return NULL;
}

SDL_Thread *SDL_CreateThreadRuntime(SDL_ThreadFunction fn, const char *name, void *data,
	SDL_FunctionPointer pfnBeginThread, SDL_FunctionPointer pfnEndThread)
{
	struct guest_sdl_thread *thread = calloc(1, sizeof(*thread));

	(void)name;
	(void)pfnBeginThread;
	(void)pfnEndThread;
	if (!thread)
		return NULL;
	thread->function = fn;
	thread->data = data;
	if (pthread_create(&thread->thread, NULL, sdl_thread_main, thread) != 0)
	{
		free(thread);
		return NULL;
	}
	return (SDL_Thread *)thread;
}

void SDL_WaitThread(SDL_Thread *handle, int *status)
{
	struct guest_sdl_thread *thread = (struct guest_sdl_thread *)handle;

	if (!thread)
		return;
	pthread_join(thread->thread, NULL);
	if (status)
		*status = thread->status;
	free(thread);
}

void SDL_DetachThread(SDL_Thread *handle)
{
	struct guest_sdl_thread *thread = (struct guest_sdl_thread *)handle;

	/* the structure is leaked: the thread may still use it */
	if (thread)
		pthread_detach(thread->thread);
}

/* ---------- files */

SDL_IOStream *SDL_IOFromFile(const char *file, const char *mode)
{
	return (SDL_IOStream *)fopen(file, mode);
}

size_t SDL_ReadIO(SDL_IOStream *context, void *ptr, size_t size)
{
	return context ? fread(ptr, 1, size, (FILE *)context) : 0;
}

size_t SDL_WriteIO(SDL_IOStream *context, const void *ptr, size_t size)
{
	return context ? fwrite(ptr, 1, size, (FILE *)context) : 0;
}

Sint64 SDL_SeekIO(SDL_IOStream *context, Sint64 offset, SDL_IOWhence whence)
{
	int origin = whence == SDL_IO_SEEK_CUR ? SEEK_CUR : whence == SDL_IO_SEEK_END ? SEEK_END : SEEK_SET;

	if (!context || fseeko((FILE *)context, (off_t)offset, origin) != 0)
		return -1;
	return (Sint64)ftello((FILE *)context);
}

Sint64 SDL_TellIO(SDL_IOStream *context)
{
	return context ? (Sint64)ftello((FILE *)context) : -1;
}

Sint64 SDL_GetIOSize(SDL_IOStream *context)
{
	FILE *file = (FILE *)context;
	off_t position, size;

	if (!file)
		return -1;
	position = ftello(file);
	if (fseeko(file, 0, SEEK_END) != 0)
		return -1;
	size = ftello(file);
	fseeko(file, position, SEEK_SET);
	return (Sint64)size;
}

bool SDL_CloseIO(SDL_IOStream *context)
{
	return context ? fclose((FILE *)context) == 0 : true;
}

void *SDL_LoadFile(const char *file, size_t *datasize)
{
	FILE *stream = fopen(file, "rb");
	char *data = NULL;
	size_t size = 0, capacity = 0;

	if (!stream)
		return NULL;
	for (;;)
	{
		size_t read;

		if (size + 65536 + 1 > capacity)
		{
			char *grown;

			capacity = (size + 65536 + 1) * 2;
			grown = realloc(data, capacity);
			if (!grown)
			{
				free(data);
				fclose(stream);
				return NULL;
			}
			data = grown;
		}
		read = fread(data + size, 1, 65536, stream);
		size += read;
		if (read < 65536)
			break;
	}
	fclose(stream);
	/* SDL terminates the data */
	data[size] = 0;
	if (datasize)
		*datasize = size;
	return data;
}

bool SDL_SaveFile(const char *file, const void *data, size_t datasize)
{
	FILE *stream = fopen(file, "wb");
	bool result;

	if (!stream)
		return false;
	result = fwrite(data, 1, datasize, stream) == datasize;
	return fclose(stream) == 0 && result;
}

const char *SDL_GetBasePath(void)
{
	/* the game's folder, with its separator (host_main.c) */
	const char *path = getenv("HALO_BASE_PATH");

	return path ? path : "./";
}

/* ---------- windows and displays */

void SDL_DestroyWindow(SDL_Window *window)
{
	host_sdl_destroy_window((unsigned int)window);
}

SDL_WindowFlags SDL_GetWindowFlags(SDL_Window *window)
{
	return (SDL_WindowFlags)host_sdl_get_window_flags((unsigned int)window);
}

bool SDL_SetWindowFullscreen(SDL_Window *window, bool fullscreen)
{
	return host_sdl_set_window_fullscreen((unsigned int)window, fullscreen) != 0;
}

bool SDL_GetWindowSize(SDL_Window *window, int *w, int *h)
{
	int width = 0, height = 0;
	int result = host_sdl_get_window_size((unsigned int)window, &width, &height);

	if (w)
		*w = width;
	if (h)
		*h = height;
	return result != 0;
}

void SDL_WarpMouseInWindow(SDL_Window *window, float x, float y)
{
	host_sdl_warp_mouse_in_window((unsigned int)window, x, y);
}

SDL_DisplayID SDL_GetPrimaryDisplay(void)
{
	return host_sdl_get_primary_display();
}

SDL_DisplayID SDL_GetDisplayForWindow(SDL_Window *window)
{
	return host_sdl_get_display_for_window((unsigned int)window);
}

const SDL_DisplayMode *SDL_GetDesktopDisplayMode(SDL_DisplayID displayID)
{
	static __thread SDL_DisplayMode mode;
	int fields[5];
	float reals[2];

	if (!host_sdl_get_desktop_display_mode(displayID, fields, reals))
		return NULL;
	memset(&mode, 0, sizeof(mode));
	mode.displayID = displayID;
	mode.format = (SDL_PixelFormat)fields[0];
	mode.w = fields[1];
	mode.h = fields[2];
	mode.refresh_rate_numerator = fields[3];
	mode.refresh_rate_denominator = fields[4];
	mode.pixel_density = reals[0];
	mode.refresh_rate = reals[1];
	return &mode;
}

/* ---------- the open file dialog

The host keeps the chosen path; the guest's callback runs on the guest's
own thread, from the event functions it waits with. */

static SDL_DialogFileCallback dialog_callback;
static void *dialog_userdata;

static void dialog_poll(void)
{
	char path[1024];
	const char *files[2];
	int state;
	SDL_DialogFileCallback callback = dialog_callback;

	if (!callback)
		return;
	state = host_sdl_file_dialog_result(path, sizeof(path));
	if (state == 0)
		return;
	dialog_callback = NULL;
	files[0] = state > 0 ? path : NULL;
	files[1] = NULL;
	callback(dialog_userdata, files, -1);
}

void SDL_ShowOpenFileDialog(SDL_DialogFileCallback callback, void *userdata, SDL_Window *window,
	const SDL_DialogFileFilter *filters, int nfilters, const char *default_location, bool allow_many)
{
	char packed[1024];
	size_t used = 0;
	int index, count = 0;

	(void)window;
	(void)default_location;
	(void)allow_many;
	for (index = 0; index < nfilters; index++)
	{
		size_t name = strlen(filters[index].name) + 1, pattern = strlen(filters[index].pattern) + 1;

		if (used + name + pattern > sizeof(packed))
			break;
		memcpy(packed + used, filters[index].name, name);
		used += name;
		memcpy(packed + used, filters[index].pattern, pattern);
		used += pattern;
		count++;
	}
	dialog_callback = callback;
	dialog_userdata = userdata;
	host_sdl_show_open_file_dialog(packed, count);
	dialog_poll();
}

bool SDL_WaitEventTimeout(SDL_Event *event, Sint32 timeoutMS)
{
	int result = host_sdl_wait_event_timeout(event, timeoutMS);

	dialog_poll();
	return result != 0;
}

void SDL_PumpEvents(void)
{
	/* (the host's SDL_WaitEventTimeout without an event pumps them) */
	host_sdl_wait_event_timeout(NULL, 0);
	dialog_poll();
}

/* ---------- message boxes */

bool SDL_ShowMessageBox(const SDL_MessageBoxData *messageboxdata, int *buttonid)
{
	int flags[8], ids[8];
	char texts[1024];
	size_t used = 0;
	int index, count = 0, chosen = -1, result;

	for (index = 0; index < messageboxdata->numbuttons && index < 8; index++)
	{
		const char *text = messageboxdata->buttons[index].text ? messageboxdata->buttons[index].text : "";
		size_t length = strlen(text) + 1;

		if (used + length > sizeof(texts))
			break;
		flags[index] = (int)messageboxdata->buttons[index].flags;
		ids[index] = messageboxdata->buttons[index].buttonID;
		memcpy(texts + used, text, length);
		used += length;
		count++;
	}
	result = host_sdl_show_message_box((unsigned int)messageboxdata->flags, messageboxdata->title,
		messageboxdata->message, count, flags, ids, texts, &chosen);
	if (buttonid)
		*buttonid = chosen;
	return result != 0;
}

/* ---------- the renderer (the first start's extraction progress) */

SDL_Renderer *SDL_CreateRenderer(SDL_Window *window, const char *name)
{
	(void)name;
	return (SDL_Renderer *)host_sdl_create_renderer((unsigned int)window);
}

void SDL_DestroyRenderer(SDL_Renderer *renderer)
{
	host_sdl_destroy_renderer((unsigned int)renderer);
}

bool SDL_SetRenderVSync(SDL_Renderer *renderer, int vsync)
{
	return host_sdl_set_render_vsync((unsigned int)renderer, vsync) != 0;
}

bool SDL_SetRenderDrawColor(SDL_Renderer *renderer, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
	return host_sdl_set_render_draw_color((unsigned int)renderer, r, g, b, a) != 0;
}

bool SDL_SetRenderScale(SDL_Renderer *renderer, float scaleX, float scaleY)
{
	return host_sdl_set_render_scale((unsigned int)renderer, scaleX, scaleY) != 0;
}

bool SDL_RenderClear(SDL_Renderer *renderer)
{
	return host_sdl_render_clear((unsigned int)renderer) != 0;
}

bool SDL_RenderFillRect(SDL_Renderer *renderer, const SDL_FRect *rect)
{
	return host_sdl_render_fill_rect((unsigned int)renderer, (const float *)rect) != 0;
}

bool SDL_RenderDebugText(SDL_Renderer *renderer, float x, float y, const char *str)
{
	return host_sdl_render_debug_text((unsigned int)renderer, x, y, str) != 0;
}

bool SDL_RenderPresent(SDL_Renderer *renderer)
{
	return host_sdl_render_present((unsigned int)renderer) != 0;
}
