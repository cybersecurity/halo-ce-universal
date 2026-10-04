/*
GUEST_DESKTOP.C

The guest's half of what the Linux desktop's code paths need beyond the
Android app's (port/android/guest/runtime/guest_sdl.c): the SDL3 functions
sdl_platform.c, menu_files.c, xiso.c and updater.c call, and update.h's for
the self-updater. host_desktop.c is the host's half.

What the guest can do itself it does here, on its own threads and its own C
library: SDL's threads, mutexes and atomics (a thread that runs guest code
must be a guest thread), its files (SDL_IOStream is a stdio FILE here) and
SDL_GlobDirectory. The rest goes to the host, with SDL's objects as handles
and its structures copied field by field where they hold pointers.
*/

/* before SDL: musl's alloca.h must come first */
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <SDL3/SDL.h>

#include "update.h"

/* ---------- the host's half (host_desktop.c, host_imports.list) */

void host_sdl_get_base_path(char *buffer, unsigned int size);
long long host_sdl_ticks_ns(void);
void host_sdl_pump_events(void);
int host_sdl_push_event(const void *event);
long long host_sdl_get_window_flags(unsigned int window);
int host_sdl_set_window_fullscreen(unsigned int window, int fullscreen);
int host_sdl_set_window_fullscreen_mode(unsigned int window, unsigned int display, int width, int height,
	float refresh_rate);
void host_sdl_get_window_size(unsigned int window, int *width, int *height);
int host_sdl_set_window_size(unsigned int window, int width, int height);
void host_sdl_destroy_window(unsigned int window);
int host_sdl_warp_mouse_in_window(unsigned int window, float x, float y);
unsigned int host_sdl_get_display_for_window(unsigned int window);
unsigned int host_sdl_get_primary_display(void);
int host_sdl_get_display_mode(unsigned int display, int current, void *result);
unsigned int host_sdl_create_renderer(unsigned int window, const char *name);
void host_sdl_destroy_renderer(unsigned int renderer);
int host_sdl_set_render_vsync(unsigned int renderer, int vsync);
int host_sdl_set_render_draw_color(unsigned int renderer, unsigned int red, unsigned int green, unsigned int blue,
	unsigned int alpha);
int host_sdl_set_render_scale(unsigned int renderer, float x, float y);
int host_sdl_render_clear(unsigned int renderer);
int host_sdl_render_fill_rect(unsigned int renderer, const void *rectangle);
int host_sdl_render_debug_text(unsigned int renderer, float x, float y, const char *text);
int host_sdl_render_present(unsigned int renderer);
int host_sdl_show_message_box(unsigned int flags, const char *title, const char *message, int button_count,
	const int *button_flags, const int *button_ids, const char *button_texts, int *chosen);
int host_sdl_open_file_dialog(int filter_count, const char *filter_names, const char *filter_patterns,
	char *path, unsigned int size);
int host_update_download(const char *url, const char *path, unsigned int progress, unsigned int context,
	char *error, int error_size);
int host_update_executable_path(char *path, int size);
int host_update_replace_file(const char *path, const char *new_path, const char *old_path);
void host_update_delete_file(const char *path);
int host_update_make_directory(const char *path);
int host_update_launch(const char *path);

/* ---------- general */

const char *SDL_GetBasePath(void)
{
	static char path[1024];

	if (!path[0])
		host_sdl_get_base_path(path, sizeof(path));
	return path;
}

Uint64 SDL_GetTicksNS(void)
{
	return (Uint64)host_sdl_ticks_ns();
}

void SDL_DelayPrecise(Uint64 nanoseconds)
{
	struct timespec duration;

	duration.tv_sec = (time_t)(nanoseconds / 1000000000ULL);
	duration.tv_nsec = (long)(nanoseconds % 1000000000ULL);
	while (nanosleep(&duration, &duration) != 0 && errno == EINTR)
		;
}

void SDL_PumpEvents(void)
{
	host_sdl_pump_events();
}

bool SDL_PushEvent(SDL_Event *event)
{
	return host_sdl_push_event(event) != 0;
}

/* ---------- atomics, mutexes and threads, the guest's own */

int SDL_GetAtomicInt(SDL_AtomicInt *atomic)
{
	return __atomic_load_n(&atomic->value, __ATOMIC_SEQ_CST);
}

int SDL_SetAtomicInt(SDL_AtomicInt *atomic, int value)
{
	return __atomic_exchange_n(&atomic->value, value, __ATOMIC_SEQ_CST);
}

SDL_Mutex *SDL_CreateMutex(void)
{
	pthread_mutex_t *mutex = malloc(sizeof(*mutex));

	if (mutex && pthread_mutex_init(mutex, NULL) != 0)
	{
		free(mutex);
		mutex = NULL;
	}
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
	if (!mutex)
		return;
	pthread_mutex_destroy((pthread_mutex_t *)mutex);
	free(mutex);
}

struct guest_sdl_thread
{
	pthread_t thread;
	SDL_ThreadFunction function;
	void *data;
	int status;
};

static void *guest_sdl_thread_start(void *argument)
{
	struct guest_sdl_thread *thread = argument;

	thread->status = thread->function(thread->data);
	return NULL;
}

/* (what SDL_CreateThread expands to) */
SDL_Thread *SDL_CreateThreadRuntime(SDL_ThreadFunction function, const char *name, void *data,
	SDL_FunctionPointer begin_thread, SDL_FunctionPointer end_thread)
{
	struct guest_sdl_thread *thread = calloc(1, sizeof(*thread));

	(void)name;
	(void)begin_thread;
	(void)end_thread;
	if (!thread)
		return NULL;
	thread->function = function;
	thread->data = data;
	if (pthread_create(&thread->thread, NULL, guest_sdl_thread_start, thread) != 0)
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

/* ---------- files: an SDL_IOStream is a stdio FILE */

SDL_IOStream *SDL_IOFromFile(const char *path, const char *mode)
{
	return (SDL_IOStream *)fopen(path, mode);
}

bool SDL_CloseIO(SDL_IOStream *stream)
{
	return stream && fclose((FILE *)stream) == 0;
}

size_t SDL_ReadIO(SDL_IOStream *stream, void *buffer, size_t size)
{
	return fread(buffer, 1, size, (FILE *)stream);
}

size_t SDL_WriteIO(SDL_IOStream *stream, const void *buffer, size_t size)
{
	return fwrite(buffer, 1, size, (FILE *)stream);
}

Sint64 SDL_SeekIO(SDL_IOStream *stream, Sint64 offset, SDL_IOWhence whence)
{
	int origin = whence == SDL_IO_SEEK_CUR ? SEEK_CUR : whence == SDL_IO_SEEK_END ? SEEK_END : SEEK_SET;

	if (fseeko((FILE *)stream, (off_t)offset, origin) != 0)
		return -1;
	return (Sint64)ftello((FILE *)stream);
}

Sint64 SDL_TellIO(SDL_IOStream *stream)
{
	return (Sint64)ftello((FILE *)stream);
}

Sint64 SDL_GetIOSize(SDL_IOStream *stream)
{
	struct stat information;

	if (fstat(fileno((FILE *)stream), &information) != 0)
		return -1;
	return (Sint64)information.st_size;
}

/* ---------- SDL_GlobDirectory: * and ? within a name, as SDL's */

static int glob_match(const char *pattern, const char *name)
{
	for (; *pattern; pattern++, name++)
	{
		if (*pattern == '*')
		{
			for (pattern++; ; name++)
			{
				if (glob_match(pattern, name))
					return 1;
				if (!*name)
					return 0;
			}
		}
		if (!*name || (*pattern != '?' && *pattern != *name))
			return 0;
	}
	return !*name;
}

struct glob_result
{
	char **names;
	int count, capacity;
	size_t text_size;
};

static void glob_add(struct glob_result *result, const char *name)
{
	if (result->count == result->capacity)
	{
		int capacity = result->capacity ? result->capacity * 2 : 16;
		char **names = realloc(result->names, (size_t)capacity * sizeof(char *));

		if (!names)
			return;
		result->names = names;
		result->capacity = capacity;
	}
	result->names[result->count] = strdup(name);
	if (result->names[result->count])
		result->text_size += strlen(name) + 1, result->count++;
}

/* the names under folder (relative to root, prefix) matching the pattern's
components from here on */
static void glob_folder(struct glob_result *result, const char *root, const char *prefix, const char *pattern)
{
	const char *slash = strchr(pattern, '/');
	size_t component_length = slash ? (size_t)(slash - pattern) : strlen(pattern);
	char component[256];
	char folder[1024];
	DIR *directory;
	struct dirent *entry;

	if (component_length >= sizeof(component))
		return;
	memcpy(component, pattern, component_length);
	component[component_length] = 0;
	snprintf(folder, sizeof(folder), "%s%s%s", root, *prefix ? "/" : "", prefix);
	directory = opendir(folder);
	if (!directory)
		return;
	while ((entry = readdir(directory)))
	{
		char name[1024];

		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") || !glob_match(component, entry->d_name))
			continue;
		snprintf(name, sizeof(name), "%s%s%s", prefix, *prefix ? "/" : "", entry->d_name);
		if (slash)
			glob_folder(result, root, name, slash + 1);
		else
			glob_add(result, name);
	}
	closedir(directory);
}

/* one allocation, the array then the names, which SDL_free frees */
char **SDL_GlobDirectory(const char *path, const char *pattern, SDL_GlobFlags flags, int *count)
{
	struct glob_result result = { NULL, 0, 0, 0 };
	char **names = NULL;
	int index;

	(void)flags;
	glob_folder(&result, path, "", pattern ? pattern : "*");
	names = malloc((size_t)(result.count + 1) * sizeof(char *) + result.text_size);
	if (names)
	{
		char *text = (char *)(names + result.count + 1);

		for (index = 0; index < result.count; index++)
		{
			size_t length = strlen(result.names[index]) + 1;

			memcpy(text, result.names[index], length);
			names[index] = text;
			text += length;
		}
		names[result.count] = NULL;
	}
	for (index = 0; index < result.count; index++)
		free(result.names[index]);
	free(result.names);
	if (count)
		*count = names ? result.count : 0;
	return names;
}

/* ---------- windows and displays */

#define WINDOW(window) ((unsigned int)(uintptr_t)(window))

SDL_WindowFlags SDL_GetWindowFlags(SDL_Window *window)
{
	return (SDL_WindowFlags)host_sdl_get_window_flags(WINDOW(window));
}

bool SDL_SetWindowFullscreen(SDL_Window *window, bool fullscreen)
{
	return host_sdl_set_window_fullscreen(WINDOW(window), fullscreen) != 0;
}

bool SDL_SetWindowFullscreenMode(SDL_Window *window, const SDL_DisplayMode *mode)
{
	if (!mode)
		return host_sdl_set_window_fullscreen_mode(WINDOW(window), 0, 0, 0, 0.0f) != 0;
	return host_sdl_set_window_fullscreen_mode(WINDOW(window), (unsigned int)mode->displayID, mode->w, mode->h,
		mode->refresh_rate) != 0;
}

bool SDL_GetWindowSize(SDL_Window *window, int *width, int *height)
{
	int w = 0, h = 0;

	host_sdl_get_window_size(WINDOW(window), &w, &h);
	if (width)
		*width = w;
	if (height)
		*height = h;
	return w > 0;
}

bool SDL_SetWindowSize(SDL_Window *window, int width, int height)
{
	return host_sdl_set_window_size(WINDOW(window), width, height) != 0;
}

void SDL_DestroyWindow(SDL_Window *window)
{
	host_sdl_destroy_window(WINDOW(window));
}

void SDL_WarpMouseInWindow(SDL_Window *window, float x, float y)
{
	host_sdl_warp_mouse_in_window(WINDOW(window), x, y);
}

SDL_DisplayID SDL_GetDisplayForWindow(SDL_Window *window)
{
	return (SDL_DisplayID)host_sdl_get_display_for_window(WINDOW(window));
}

SDL_DisplayID SDL_GetPrimaryDisplay(void)
{
	return (SDL_DisplayID)host_sdl_get_primary_display();
}

/* host_desktop.c's struct host_display_mode */
struct display_mode
{
	unsigned int display;
	unsigned int format;
	int width, height;
	float pixel_density;
	float refresh_rate;
	int refresh_rate_numerator, refresh_rate_denominator;
};

static const SDL_DisplayMode *display_mode(SDL_DisplayID display, int current)
{
	/* (SDL keeps the modes it returns; these last until the next call on
	this thread) */
	static __thread SDL_DisplayMode modes[2];
	SDL_DisplayMode *mode = &modes[current != 0];
	struct display_mode copy;

	if (!host_sdl_get_display_mode((unsigned int)display, current, &copy))
		return NULL;
	memset(mode, 0, sizeof(*mode));
	mode->displayID = (SDL_DisplayID)copy.display;
	mode->format = (SDL_PixelFormat)copy.format;
	mode->w = copy.width;
	mode->h = copy.height;
	mode->pixel_density = copy.pixel_density;
	mode->refresh_rate = copy.refresh_rate;
	mode->refresh_rate_numerator = copy.refresh_rate_numerator;
	mode->refresh_rate_denominator = copy.refresh_rate_denominator;
	return mode;
}

const SDL_DisplayMode *SDL_GetDesktopDisplayMode(SDL_DisplayID display)
{
	return display_mode(display, 0);
}

const SDL_DisplayMode *SDL_GetCurrentDisplayMode(SDL_DisplayID display)
{
	return display_mode(display, 1);
}

/* ---------- the 2D renderer */

#define RENDERER(renderer) ((unsigned int)(uintptr_t)(renderer))

SDL_Renderer *SDL_CreateRenderer(SDL_Window *window, const char *name)
{
	return (SDL_Renderer *)(uintptr_t)host_sdl_create_renderer(WINDOW(window), name);
}

void SDL_DestroyRenderer(SDL_Renderer *renderer)
{
	host_sdl_destroy_renderer(RENDERER(renderer));
}

bool SDL_SetRenderVSync(SDL_Renderer *renderer, int vsync)
{
	return host_sdl_set_render_vsync(RENDERER(renderer), vsync) != 0;
}

bool SDL_SetRenderDrawColor(SDL_Renderer *renderer, Uint8 red, Uint8 green, Uint8 blue, Uint8 alpha)
{
	return host_sdl_set_render_draw_color(RENDERER(renderer), red, green, blue, alpha) != 0;
}

bool SDL_SetRenderScale(SDL_Renderer *renderer, float x, float y)
{
	return host_sdl_set_render_scale(RENDERER(renderer), x, y) != 0;
}

bool SDL_RenderClear(SDL_Renderer *renderer)
{
	return host_sdl_render_clear(RENDERER(renderer)) != 0;
}

bool SDL_RenderFillRect(SDL_Renderer *renderer, const SDL_FRect *rectangle)
{
	return host_sdl_render_fill_rect(RENDERER(renderer), rectangle) != 0;
}

bool SDL_RenderDebugText(SDL_Renderer *renderer, float x, float y, const char *text)
{
	return host_sdl_render_debug_text(RENDERER(renderer), x, y, text) != 0;
}

bool SDL_RenderPresent(SDL_Renderer *renderer)
{
	return host_sdl_render_present(RENDERER(renderer)) != 0;
}

/* ---------- dialogs */

#define MESSAGE_BOX_BUTTONS 8

bool SDL_ShowMessageBox(const SDL_MessageBoxData *data, int *button)
{
	int flags[MESSAGE_BOX_BUTTONS], ids[MESSAGE_BOX_BUTTONS];
	char texts[1024];
	size_t used = 0;
	int count = data->numbuttons < MESSAGE_BOX_BUTTONS ? data->numbuttons : MESSAGE_BOX_BUTTONS;
	int index;
	int chosen = -1;

	for (index = 0; index < count; index++)
	{
		const char *text = data->buttons[index].text ? data->buttons[index].text : "";
		size_t length = strlen(text) + 1;

		if (used + length > sizeof(texts))
		{
			count = index;
			break;
		}
		flags[index] = (int)data->buttons[index].flags;
		ids[index] = data->buttons[index].buttonID;
		memcpy(texts + used, text, length);
		used += length;
	}
	if (!host_sdl_show_message_box((unsigned int)data->flags, data->title ? data->title : "",
		data->message ? data->message : "", count, flags, ids, texts, &chosen))
	{
		return false;
	}
	if (button)
		*button = chosen;
	return true;
}

/* the host waits for the dialog, then the callback runs here, before this
returns (sdl_platform.c waits for it either way) */
void SDL_ShowOpenFileDialog(SDL_DialogFileCallback callback, void *userdata, SDL_Window *window,
	const SDL_DialogFileFilter *filters, int filter_count, const char *default_location, bool allow_many)
{
	char names[512], patterns[512];
	size_t names_used = 0, patterns_used = 0;
	char path[1024];
	const char *files[2] = { path, NULL };
	int count = 0;
	int index;

	(void)window;
	(void)default_location;
	(void)allow_many;
	for (index = 0; index < filter_count && index < 8; index++)
	{
		size_t name_length = strlen(filters[index].name) + 1;
		size_t pattern_length = strlen(filters[index].pattern) + 1;

		if (names_used + name_length > sizeof(names) || patterns_used + pattern_length > sizeof(patterns))
			break;
		memcpy(names + names_used, filters[index].name, name_length);
		memcpy(patterns + patterns_used, filters[index].pattern, pattern_length);
		names_used += name_length;
		patterns_used += pattern_length;
		count++;
	}
	if (host_sdl_open_file_dialog(count, names, patterns, path, sizeof(path)))
		callback(userdata, files, -1);
	else
		callback(userdata, files + 1, -1);
}

/* ---------- the self-updater (update.h): the host's posix_update.c */

struct download_progress
{
	update_progress_proc progress;
	void *context;
};

/* called by the host on this thread (host_desktop.c, download_progress) */
static void download_progress(unsigned int context, unsigned int received, unsigned int total)
{
	struct download_progress *binding = (struct download_progress *)(uintptr_t)context;

	binding->progress(binding->context, received, total);
}

int update_download(const char *url, const char *path, update_progress_proc progress, void *context,
	char *error, int error_size)
{
	struct download_progress binding = { progress, context };

	return host_update_download(url, path, progress ? (unsigned int)(uintptr_t)download_progress : 0,
		(unsigned int)(uintptr_t)&binding, error, error_size);
}

int update_executable_path(char *path, int size)
{
	return host_update_executable_path(path, size);
}

int update_replace_file(const char *path, const char *new_path, const char *old_path)
{
	return host_update_replace_file(path, new_path, old_path);
}

void update_delete_file(const char *path)
{
	host_update_delete_file(path);
}

int update_make_directory(const char *path)
{
	return host_update_make_directory(path);
}

int update_launch(const char *path)
{
	return host_update_launch(path);
}
