/*
SDL_PLATFORM.C

The SDL3 window, OpenGL context and event loop behind the Linux build.

The window is created with the Direct3D device (d3d8_gl.c) on the game's
main thread, which is also the only thread that pumps events. Keyboard and
mouse state gathered here feeds the controller emulation in xinput_sdl.c
and the debug keyboard that the game's console reads.
*/

#include "platform.h"
#include "sdl_platform.h"
#include "gl.h"
#include "port_config.h"
#include "p2p.h"
#include "xiso.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32) && !defined(HALO_ANDROID)
#include <signal.h>
#endif

static SDL_Window *platform_window;
static SDL_GLContext platform_gl_context;
static SDL_ThreadID platform_event_thread;
static BOOL platform_sdl_started = FALSE;

static struct platform_input_state input_state;
/* keys pressed since the last read, so a press and release between two
reads still counts as a press (input injected on Android, or a slow frame) */
static unsigned char keys_pressed[SDL_SCANCODE_COUNT];
/* likewise the mouse buttons pressed since the last read, so that a click
quicker than a frame still counts */
static unsigned char mouse_buttons_pressed[PLATFORM_MOUSE_BUTTON_COUNT];
#ifndef HALO_ANDROID
/* the menus' pointer (platform_ui_pointer_set_active), under input_lock */
static struct platform_ui_pointer ui_pointer;
static float ui_pointer_wheel;
#endif
static pthread_mutex_t input_lock = PTHREAD_MUTEX_INITIALIZER;
/* rebinding (platform_binding_capture_begin), under input_lock: waiting for
an input, and the one taken; then the keyboard and mouse settle (their keys
and buttons held then are let go) before they drive anything again */
enum
{
	_binding_capture_idle,
	_binding_capture_waiting,
	_binding_capture_taken,
};
static int binding_capture;
static int binding_capture_result;
static int binding_captured_input;
static BOOL binding_settling;
static Uint64 binding_taken_ms;
/* when the menus last asked (a capture they stop asking about, their screen
gone, ends: else the keyboard stays held from the game) */
static Uint64 binding_polled_ms;
#define BINDING_ABANDONED_MS 500
static unsigned mouse_buttons_down;
/* (an input taken that nothing asks for is let go after this) */
#define BINDING_UNCLAIMED_MS 2000
/* the multiplayer scoreboard is open (platform_scoreboard_scroll): the wheel
and Page Up/Down scroll it, and the wheel switches no weapon; how far they
have moved it since the game last asked (notches down, pages down). Open
until the game stops saying so for SCOREBOARD_OPEN_MS (a game that ends with
it open never says it closed). */
#define SCOREBOARD_OPEN_MS 250
static Uint64 scoreboard_open_until_ms;
static float scoreboard_wheel;
static long scoreboard_notches;
static long scoreboard_pages;

/* debug keyboard queue */
#define KEYSTROKE_QUEUE_SIZE 64
static struct platform_keystroke keystroke_queue[KEYSTROKE_QUEUE_SIZE];
static unsigned long keystroke_head, keystroke_count;

#ifndef HALO_ANDROID
/* updater.c's: the desktop self-updater */
void updater_start(void);
void updater_poll(SDL_Window *window);
#endif

BOOL platform_sdl_initialize(void)
{
	if (platform_sdl_started)
		return TRUE;
#if !defined(_WIN32) && !defined(HALO_ANDROID)
	/* a write to a connection the other end closed fails instead of ending
	the game (the game's sockets and Discord's pass MSG_NOSIGNAL, but UPnP's
	miniupnpc does not, nor does a write to a closed pipe's standard error) */
	signal(SIGPIPE, SIG_IGN);
#endif
	/* a copy of the game started to open an invite link hands it to the
	one already running, and goes */
	if (p2p_hand_off_invite())
		exit(EXIT_SUCCESS);
	SDL_SetHint(SDL_HINT_APP_NAME, "Halo");
#ifdef HALO_ANDROID
	/* landscape only; the back key arrives as a key event (xinput_sdl.c)
	instead of closing the activity */
	SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
	SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
	/* touching the screen must not aim or fire (the mouse drives the
	controller emulation in xinput_sdl.c) */
	SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
#endif
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS))
	{
		platform_log("SDL_Init failed: %s", SDL_GetError());
		return FALSE;
	}
	platform_sdl_started = TRUE;
#ifndef HALO_ANDROID
	/* found (or offered to the player, platform_offer_game_data) before the
	game's window opens */
	platform_data_root();
	/* (a new version looked for meanwhile, updater_poll asking about it) */
	updater_start();
#endif
	return TRUE;
}

#ifndef HALO_ANDROID
/* ---------- first start without game data (xbox_files.c) */

struct data_extraction
{
	pthread_mutex_t lock;
	char image[1024];
	char destination[1024];
	char file[256];
	unsigned long long done;
	unsigned long long total;
	BOOL finished;
	BOOL succeeded;
	char error[512];
};

static void data_extraction_progress(void *context, const char *file, unsigned long long done,
	unsigned long long total)
{
	struct data_extraction *extraction = context;

	pthread_mutex_lock(&extraction->lock);
	snprintf(extraction->file, sizeof(extraction->file), "%s", file);
	extraction->done = done;
	extraction->total = total;
	pthread_mutex_unlock(&extraction->lock);
}

static void *data_extraction_thread(void *context)
{
	struct data_extraction *extraction = context;
	BOOL succeeded = xiso_extract_maps(extraction->image, extraction->destination, data_extraction_progress,
		extraction, extraction->error, sizeof(extraction->error)) != 0;

	pthread_mutex_lock(&extraction->lock);
	extraction->succeeded = succeeded;
	extraction->finished = TRUE;
	pthread_mutex_unlock(&extraction->lock);
	return NULL;
}

/* copies the maps, showing how far it has got; closing the window quits */
static BOOL data_extract(const char *image, const char *destination, char *error, int error_size)
{
	static struct data_extraction extraction;
	SDL_Window *window;
	SDL_Renderer *renderer = NULL;
	pthread_t thread;
	BOOL finished = FALSE;

	memset(&extraction, 0, sizeof(extraction));
	pthread_mutex_init(&extraction.lock, NULL);
	snprintf(extraction.image, sizeof(extraction.image), "%s", image);
	snprintf(extraction.destination, sizeof(extraction.destination), "%s", destination);
	if (pthread_create(&thread, NULL, data_extraction_thread, &extraction) != 0)
	{
		snprintf(error, (size_t)error_size, "Could not start the extraction.");
		return FALSE;
	}
	/* (waited for through extraction.finished; the Windows port's threads
	cannot be joined) */
	pthread_detach(thread);
	window = SDL_CreateWindow("Halo", 640, 150, 0);
	if (window)
	{
		renderer = SDL_CreateRenderer(window, NULL);
		if (renderer)
			SDL_SetRenderVSync(renderer, 1);
	}
	while (!finished)
	{
		SDL_Event event;
		char file[256];
		unsigned long long done, total;

		while (SDL_PollEvent(&event))
		{
			if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
			{
				platform_log("extraction cancelled");
				exit(EXIT_SUCCESS);
			}
		}
		pthread_mutex_lock(&extraction.lock);
		finished = extraction.finished;
		snprintf(file, sizeof(file), "%s", extraction.file);
		done = extraction.done;
		total = extraction.total;
		pthread_mutex_unlock(&extraction.lock);
		if (renderer)
		{
			char line[320];
			SDL_FRect bar = { 20.0f, 100.0f, 600.0f, 24.0f };
			float fraction = total ? (float)((double)done / (double)total) : 0.0f;

			SDL_SetRenderDrawColor(renderer, 12, 16, 20, 255);
			SDL_RenderClear(renderer);
			SDL_SetRenderDrawColor(renderer, 230, 230, 230, 255);
			SDL_SetRenderScale(renderer, 2.0f, 2.0f);
			SDL_RenderDebugText(renderer, 10.0f, 10.0f, "Extracting the maps folder...");
			SDL_SetRenderScale(renderer, 1.0f, 1.0f);
			snprintf(line, sizeof(line), "%s  (%llu of %llu MB)", file, done >> 20, total >> 20);
			SDL_RenderDebugText(renderer, 20.0f, 70.0f, line);
			SDL_SetRenderDrawColor(renderer, 60, 66, 72, 255);
			SDL_RenderFillRect(renderer, &bar);
			bar.w *= fraction;
			SDL_SetRenderDrawColor(renderer, 90, 160, 90, 255);
			SDL_RenderFillRect(renderer, &bar);
			SDL_RenderPresent(renderer);
		}
		SDL_Delay(16);
	}
	if (renderer)
		SDL_DestroyRenderer(renderer);
	if (window)
		SDL_DestroyWindow(window);
	if (!extraction.succeeded)
		snprintf(error, (size_t)error_size, "%s", extraction.error);
	return extraction.succeeded;
}

struct data_image_choice
{
	SDL_AtomicInt done;
	char path[1024];
};

static void SDLCALL data_image_chosen(void *userdata, const char * const *files, int filter)
{
	struct data_image_choice *choice = userdata;

	(void)filter;
	if (files && files[0])
		snprintf(choice->path, sizeof(choice->path), "%s", files[0]);
	SDL_SetAtomicInt(&choice->done, 1);
}

/* the disc image the player picks; FALSE if they pick none */
static BOOL data_choose_image(char *path, int size)
{
	static const SDL_DialogFileFilter filters[] =
	{
		{ "Xbox disc images", "iso;xiso" },
		{ "All files", "*" },
	};
	static struct data_image_choice choice;

	memset(&choice, 0, sizeof(choice));
	SDL_ShowOpenFileDialog(data_image_chosen, &choice, NULL, filters, 2, NULL, false);
	/* the dialog answers through events (and on some systems another
	thread) */
	while (!SDL_GetAtomicInt(&choice.done))
	{
		SDL_PumpEvents();
		SDL_Delay(50);
	}
	if (!choice.path[0])
		return FALSE;
	snprintf(path, (size_t)size, "%s", choice.path);
	return TRUE;
}

BOOL platform_offer_game_data(const char *destination)
{
	static const SDL_MessageBoxButtonData buttons[] =
	{
		{ SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Yes" },
		{ SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "No" },
	};
	char message[1400];

	/* not for runs nobody is watching */
	if (config_boolean("debug.hidden_window") || config_real("debug.exit_after") > 0.0 ||
		!SDL_Init(SDL_INIT_VIDEO))
	{
		return FALSE;
	}
	snprintf(message, sizeof(message),
		"Halo's game data (its maps folder) was not found.\n\n"
		"Extract the maps folder from an Xbox disc image (.iso) of Halo: Combat Evolved? "
		"It is copied to %s/maps (about 2 GB).\n\n"
		"(Or put the maps folder there yourself, or set paths.data in config.toml.)",
		destination);
	for (;;)
	{
		SDL_MessageBoxData question = { SDL_MESSAGEBOX_INFORMATION, NULL, "Halo", message, 2, buttons, NULL };
		char image[1024];
		char error[512];
		int answer = 0;

		if (!SDL_ShowMessageBox(&question, &answer) || answer != 1)
		{
			platform_log("no game data: quitting");
			exit(EXIT_SUCCESS);
		}
		/* no image picked: ask again */
		if (!data_choose_image(image, sizeof(image)))
			continue;
		platform_log("extracting the maps folder from %s to %s", image, destination);
		if (data_extract(image, destination, error, sizeof(error)))
		{
			platform_log("extracted the maps folder");
			return TRUE;
		}
		platform_log("extraction failed: %s", error);
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Halo", error, NULL);
	}
}
#endif

int halo_interpolation_enabled(void)
{
	static int enabled;
	static unsigned long read_at = (unsigned long)-1;

	if (read_at != config_changes())
	{
		read_at = config_changes();
		enabled = config_boolean("display.interpolation");
	}
	return enabled;
}

#ifndef HALO_ANDROID
/* whether the window is to be fullscreen (display.mode "fullscreen", or the
older "borderless"; else display.fullscreen), never when it is hidden.
Fullscreen is SDL's without a display mode: a borderless window over the
whole desktop, whose mode is never changed. F11 switches to the window and
back. */
static BOOL platform_fullscreen_setting(void)
{
	const char *mode = config_string("display.mode");

	if (config_boolean("debug.hidden_window") || !strcmp(mode, "windowed"))
		return FALSE;
	return !strcmp(mode, "fullscreen") || !strcmp(mode, "borderless") || config_boolean("display.fullscreen");
}

/* ---------- the resolution

Fullscreen draws at the resolution set (display.resolution_width and
_height, Video Setup's Resolution), d3d8_gl.c scaling the picture to the
display, whose mode stays its desktop one; or at the display's own, where
none is set or the display the game is on has no room for it. A new one is
taken up between frames (halo_screen_commit). */

/* the display the game is on, or will open on */
static SDL_DisplayID platform_display(void)
{
	SDL_DisplayID display = platform_window ? SDL_GetDisplayForWindow(platform_window) : 0;

	return display ? display : SDL_GetPrimaryDisplay();
}

/* a display's own size in pixels (its desktop mode) */
static BOOL platform_display_size(SDL_DisplayID display, long *width, long *height)
{
	const SDL_DisplayMode *mode = display ? SDL_GetDesktopDisplayMode(display) : NULL;

	if (!mode)
		return FALSE;
	*width = (long)(mode->w * mode->pixel_density + 0.5f);
	*height = (long)(mode->h * mode->pixel_density + 0.5f);
	return TRUE;
}

/* whether the game can draw a display of the size at a resolution: the
Xbox's 640x480 or more, and no more than the display */
static BOOL platform_resolution_fits(long width, long height, long display_width, long display_height)
{
	return width >= 640 && height >= 480 && width <= display_width && height <= display_height;
}

/* whether the game is, or is to be, fullscreen, and if so the size in
pixels it draws the display at (d3d8_gl.c) */
BOOL platform_screen_mode(long *width, long *height)
{
	/* the display size last fallen back to, logged once */
	static long fallback_width, fallback_height;
	long resolution_width, resolution_height;

	if (platform_window ? !(SDL_GetWindowFlags(platform_window) & SDL_WINDOW_FULLSCREEN) :
		!platform_fullscreen_setting() || !platform_sdl_initialize())
	{
		return FALSE;
	}
	if (!platform_display_size(platform_display(), width, height))
		return FALSE;
	resolution_width = config_integer("display.resolution_width");
	resolution_height = config_integer("display.resolution_height");
	if (platform_resolution_fits(resolution_width, resolution_height, *width, *height))
	{
		fallback_width = fallback_height = 0;
		*width = resolution_width;
		*height = resolution_height;
	}
	else if ((resolution_width || resolution_height) && (*width != fallback_width || *height != fallback_height))
	{
		platform_log("display: no room for %ldx%ld on this display: drawing at its %ldx%ld", resolution_width,
			resolution_height, *width, *height);
		fallback_width = *width;
		fallback_height = *height;
	}
	return TRUE;
}

void platform_resolution_setting(int *width, int *height, int default_value)
{
	long resolution_width, resolution_height, display_width, display_height;

	if (default_value)
	{
		char text[16] = "";

		config_default("display.resolution_width", text, sizeof(text));
		resolution_width = strtol(text, NULL, 10);
		config_default("display.resolution_height", text, sizeof(text));
		resolution_height = strtol(text, NULL, 10);
	}
	else
	{
		resolution_width = config_integer("display.resolution_width");
		resolution_height = config_integer("display.resolution_height");
	}
	/* (none, one the game cannot draw at, or the display's own: 0x0) */
	if (resolution_width < 640 || resolution_height < 480 || (platform_sdl_initialize() &&
		platform_display_size(platform_display(), &display_width, &display_height) &&
		resolution_width == display_width && resolution_height == display_height))
	{
		resolution_width = resolution_height = 0;
	}
	*width = (int)resolution_width;
	*height = (int)resolution_height;
}

/* the size added to the list unless it has it; the list's count */
static int platform_resolution_add(int *widths, int *heights, int count, int maximum, long width, long height)
{
	int index;

	for (index = 0; index < count; index++)
	{
		if (widths[index] == width && heights[index] == height)
			return count;
	}
	if (count >= maximum)
		return count;
	widths[count] = (int)width;
	heights[count] = (int)height;
	return count + 1;
}

int platform_display_resolutions(int *widths, int *heights, int maximum, int *native)
{
	SDL_DisplayID display;
	long display_width, display_height;
	SDL_DisplayMode **modes;
	int mode_count = 0, count = 0, set_width, set_height, index;

	*native = -1;
	if (maximum < 1 || !platform_sdl_initialize())
		return 0;
	display = platform_display();
	if (!platform_display_size(display, &display_width, &display_height))
		return 0;
	count = platform_resolution_add(widths, heights, count, maximum, display_width, display_height);
	/* (the one set, so that Video Setup shows it, even where this display has
	no room for it) */
	platform_resolution_setting(&set_width, &set_height, FALSE);
	if (set_width)
		count = platform_resolution_add(widths, heights, count, maximum, set_width, set_height);
	/* (a size once for each of its refresh rates) */
	modes = SDL_GetFullscreenDisplayModes(display, &mode_count);
	for (index = 0; modes && index < mode_count; index++)
	{
		long width = (long)(modes[index]->w * modes[index]->pixel_density + 0.5f);
		long height = (long)(modes[index]->h * modes[index]->pixel_density + 0.5f);

		if (platform_resolution_fits(width, height, display_width, display_height))
			count = platform_resolution_add(widths, heights, count, maximum, width, height);
	}
	SDL_free(modes);
	/* by width, then height */
	for (index = 1; index < count; index++)
	{
		int width = widths[index], height = heights[index], place;

		for (place = index; place > 0 && (widths[place - 1] > width ||
			(widths[place - 1] == width && heights[place - 1] > height)); place--)
		{
			widths[place] = widths[place - 1];
			heights[place] = heights[place - 1];
		}
		widths[place] = width;
		heights[place] = height;
	}
	for (index = 0; index < count; index++)
	{
		if (widths[index] == display_width && heights[index] == display_height)
			*native = index;
	}
	return count;
}

/* the window resized (display.window_scale) where it is: put back at its
place unless, at the size, it no longer fits there on the usable part of
its display, where the place the system gives it is left */
static void platform_window_resize(int width, int height)
{
	SDL_DisplayID display = SDL_GetDisplayForWindow(platform_window);
	SDL_Rect usable;
	int x = 0, y = 0, top = 0, left = 0, bottom = 0, right = 0;
	/* (a fullscreen or maximized window's place is SDL's to keep) */
	BOOL keep = !(SDL_GetWindowFlags(platform_window) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED)) &&
		SDL_GetWindowPosition(platform_window, &x, &y);

	SDL_SetWindowSize(platform_window, width, height);
	if (!keep || !display || !SDL_GetDisplayUsableBounds(display, &usable))
		return;
	/* (its frame's, where the system says) */
	if (!SDL_GetWindowBordersSize(platform_window, &top, &left, &bottom, &right))
		top = left = bottom = right = 0;
	if (x - left >= usable.x && y - top >= usable.y && x + width + right <= usable.x + usable.w &&
		y + height + bottom <= usable.y + usable.h)
	{
		SDL_SetWindowPosition(platform_window, x, y);
	}
}

#else
void platform_resolution_setting(int *width, int *height, int default_value)
{
	(void)default_value;
	*width = *height = 0;
}

int platform_display_resolutions(int *widths, int *heights, int maximum, int *native)
{
	(void)widths;
	(void)heights;
	(void)maximum;
	*native = -1;
	return 0;
}

#endif
/* the window's scale (display.window_scale, as the window was made or last
resized: platform_display_apply) */
static long platform_window_scale = -1;

BOOL platform_video_initialize(unsigned long width, unsigned long height)
{
	int scale = (int)config_integer("display.window_scale");
	int version;

	if (platform_window)
		return TRUE;
	if (!platform_sdl_initialize())
		return FALSE;
	if (scale < 1)
		scale = 1;
	platform_window_scale = scale;

#ifdef HALO_ANDROID
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
#else
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 5);
#endif
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
	if (config_boolean("debug.gl_debug"))
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
#if !defined(HALO_ANDROID) && !defined(_WIN32)
	/* Mesa's GL thread: the renderer makes thousands of GL calls a frame
	and never waits for their results, so handing them to a thread of
	their own takes a fifth of the main thread's time off it. It leaves an
	explicit mesa_glthread setting alone and other drivers ignore it. */
	setenv("mesa_glthread", "true", 0);
#endif

#ifdef HALO_ANDROID
	platform_window = SDL_CreateWindow("Halo", (int)(width * scale), (int)(height * scale),
		SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN);
#else
	/* fullscreen over the desktop unless the window is set
	(platform_fullscreen_setting), where the game draws at the resolution
	platform_screen_mode gives (d3d8_gl.c); the window size is the windowed
	mode F11 switches to and
	from, where it draws 640x480: the Xbox's 640x480 times
	display.window_scale, whatever shape the fullscreen picture has */
	(void)width;
	(void)height;
	platform_window = SDL_CreateWindow("Halo", 640 * scale, 480 * scale,
		SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
		(config_boolean("debug.hidden_window") ? SDL_WINDOW_HIDDEN : 0) |
		(platform_fullscreen_setting() ? SDL_WINDOW_FULLSCREEN : 0));
#endif
	if (!platform_window)
	{
		platform_log("SDL_CreateWindow failed: %s", SDL_GetError());
		return FALSE;
	}
	platform_gl_context = SDL_GL_CreateContext(platform_window);
#ifdef HALO_ANDROID
	/* ES 3.2 where the driver has it, otherwise the renderer makes do with
	3.0 plus extensions */
	if (!platform_gl_context)
	{
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
		platform_gl_context = SDL_GL_CreateContext(platform_window);
	}
#endif
	if (!platform_gl_context)
	{
		platform_log("cannot create an OpenGL context: %s", SDL_GetError());
		return FALSE;
	}
	SDL_GL_MakeCurrent(platform_window, platform_gl_context);
	if (!gl_functions_load())
		return FALSE;
	version = SDL_GL_SetSwapInterval(config_boolean("display.vsync") ? 1 : 0);
	(void)version;
	platform_event_thread = SDL_GetCurrentThreadID();
	platform_log("OpenGL %s on %s", (const char *)glGetString(GL_VERSION), (const char *)glGetString(GL_RENDERER));
#ifndef HALO_ANDROID
	platform_mouse_capture(TRUE);
#endif
	return TRUE;
}

/* display.fullscreen, display.window_scale (when it changes: the window can
be resized) and display.vsync, as Settings has written them */
void platform_display_apply(void)
{
#ifndef HALO_ANDROID
	BOOL fullscreen = platform_fullscreen_setting();
	long scale = config_integer("display.window_scale");

	if (!platform_window)
		return;
	if (((SDL_GetWindowFlags(platform_window) & SDL_WINDOW_FULLSCREEN) != 0) != (fullscreen != FALSE))
		SDL_SetWindowFullscreen(platform_window, fullscreen ? true : false);
	if (scale != platform_window_scale && scale >= 1)
	{
		platform_window_scale = scale;
		platform_window_resize((int)(640 * scale), (int)(480 * scale));
	}
#else
	if (!platform_window)
		return;
#endif
	SDL_GL_SetSwapInterval(config_boolean("display.vsync") ? 1 : 0);
}

void platform_video_drawable_size(int *width, int *height)
{
	SDL_GetWindowSizeInPixels(platform_window, width, height);
}

#ifndef HALO_ANDROID
/* with vsync off, the time between frames display.max_fps asks for (0:
twice the display's refresh rate), or 0 for no limit. A GPU never left idle
can hang (Intel's Raptor Lake graphics, whose reset then takes the desktop
with it); the limit gives it a rest every frame. */
static Uint64 frame_interval_ns(void)
{
	static int vsync;
	static long maximum;
	static unsigned long read_at = (unsigned long)-1;
	float rate;

	if (read_at != config_changes())
	{
		read_at = config_changes();
		vsync = config_boolean("display.vsync");
		maximum = config_integer("display.max_fps");
	}
	if (vsync || maximum < 0)
		return 0;
	rate = (float)maximum;
	if (!maximum)
	{
		SDL_DisplayID display = SDL_GetDisplayForWindow(platform_window);
		const SDL_DisplayMode *mode = display ? SDL_GetCurrentDisplayMode(display) : NULL;

		rate = 2.0f * (mode && mode->refresh_rate > 0.0f ? mode->refresh_rate : 60.0f);
	}
	return (Uint64)(1e9f / rate);
}

#endif
void platform_video_swap(void)
{
#ifndef HALO_ANDROID
	static Uint64 next_frame;
	Uint64 interval, now;

#endif
	SDL_GL_SwapWindow(platform_window);
#ifndef HALO_ANDROID
	interval = frame_interval_ns();
	if (!interval)
		return;
	now = SDL_GetTicksNS();
	if (next_frame > now)
	{
		SDL_DelayPrecise(next_frame - now);
		now = next_frame;
	}
	/* (a frame more than an interval late starts the count again) */
	next_frame = now - next_frame > interval ? now + interval : next_frame + interval;
#endif
}

void platform_mouse_capture(BOOL capture)
{
	if (platform_window)
		SDL_SetWindowRelativeMouseMode(platform_window, capture ? true : false);
}

/* ---------- keyboard translation */

/* Windows virtual key code for an SDL scancode (the Xbox debug keyboard
reports virtual keys) */
static BYTE virtual_key_from_scancode(SDL_Scancode scancode)
{
	if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z)
		return (BYTE)('A' + (scancode - SDL_SCANCODE_A));
	if (scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_9)
		return (BYTE)('1' + (scancode - SDL_SCANCODE_1));
	if (scancode >= SDL_SCANCODE_F1 && scancode <= SDL_SCANCODE_F12)
		return (BYTE)(0x70 + (scancode - SDL_SCANCODE_F1));
	if (scancode >= SDL_SCANCODE_KP_1 && scancode <= SDL_SCANCODE_KP_9)
		return (BYTE)(0x61 + (scancode - SDL_SCANCODE_KP_1));
	switch (scancode)
	{
	case SDL_SCANCODE_0: return '0';
	case SDL_SCANCODE_KP_0: return 0x60;
	case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return 0x0d;
	case SDL_SCANCODE_ESCAPE: return 0x1b;
	case SDL_SCANCODE_BACKSPACE: return 0x08;
	case SDL_SCANCODE_TAB: return 0x09;
	case SDL_SCANCODE_SPACE: return 0x20;
	case SDL_SCANCODE_MINUS: return 0xbd;
	case SDL_SCANCODE_EQUALS: return 0xbb;
	case SDL_SCANCODE_LEFTBRACKET: return 0xdb;
	case SDL_SCANCODE_RIGHTBRACKET: return 0xdd;
	case SDL_SCANCODE_BACKSLASH: return 0xdc;
	case SDL_SCANCODE_SEMICOLON: return 0xba;
	case SDL_SCANCODE_APOSTROPHE: return 0xde;
	case SDL_SCANCODE_GRAVE: return 0xc0;
	case SDL_SCANCODE_COMMA: return 0xbc;
	case SDL_SCANCODE_PERIOD: return 0xbe;
	case SDL_SCANCODE_SLASH: return 0xbf;
	case SDL_SCANCODE_CAPSLOCK: return 0x14;
	case SDL_SCANCODE_PRINTSCREEN: return 0x2c;
	case SDL_SCANCODE_SCROLLLOCK: return 0x91;
	case SDL_SCANCODE_PAUSE: return 0x13;
	case SDL_SCANCODE_INSERT: return 0x2d;
	case SDL_SCANCODE_HOME: return 0x24;
	case SDL_SCANCODE_PAGEUP: return 0x21;
	case SDL_SCANCODE_DELETE: return 0x2e;
	case SDL_SCANCODE_END: return 0x23;
	case SDL_SCANCODE_PAGEDOWN: return 0x22;
	case SDL_SCANCODE_RIGHT: return 0x27;
	case SDL_SCANCODE_LEFT: return 0x25;
	case SDL_SCANCODE_DOWN: return 0x28;
	case SDL_SCANCODE_UP: return 0x26;
	case SDL_SCANCODE_NUMLOCKCLEAR: return 0x90;
	case SDL_SCANCODE_KP_DIVIDE: return 0x6f;
	case SDL_SCANCODE_KP_MULTIPLY: return 0x6a;
	case SDL_SCANCODE_KP_MINUS: return 0x6d;
	case SDL_SCANCODE_KP_PLUS: return 0x6b;
	case SDL_SCANCODE_KP_PERIOD: return 0x6e;
	case SDL_SCANCODE_LCTRL: return 0xa2;
	case SDL_SCANCODE_RCTRL: return 0xa3;
	case SDL_SCANCODE_LSHIFT: return 0xa0;
	case SDL_SCANCODE_RSHIFT: return 0xa1;
	case SDL_SCANCODE_LALT: return 0xa4;
	case SDL_SCANCODE_RALT: return 0xa5;
	default: return 0;
	}
}

static CHAR ascii_from_key(SDL_Keycode key, SDL_Keymod modifiers)
{
	BOOL shift = (modifiers & SDL_KMOD_SHIFT) != 0;
	static const char shifted_digits[] = ")!@#$%^&*(";

	if (key >= 'a' && key <= 'z')
		return (CHAR)((shift ^ ((modifiers & SDL_KMOD_CAPS) != 0)) ? key - 32 : key);
	if (key >= '0' && key <= '9')
		return (CHAR)(shift ? shifted_digits[key - '0'] : key);
	if (key == SDLK_RETURN || key == SDLK_KP_ENTER)
		return '\r';
	if (key == SDLK_BACKSPACE)
		return '\b';
	if (key == SDLK_TAB)
		return '\t';
	if (key == SDLK_ESCAPE)
		return 0x1b;
	if (key >= 32 && key < 127)
	{
		if (!shift)
			return (CHAR)key;
		switch (key)
		{
		case '-': return '_';
		case '=': return '+';
		case '[': return '{';
		case ']': return '}';
		case '\\': return '|';
		case ';': return ':';
		case '\'': return '"';
		case ',': return '<';
		case '.': return '>';
		case '/': return '?';
		case '`': return '~';
		default: return (CHAR)key;
		}
	}
	return 0;
}

static void queue_keystroke(const SDL_KeyboardEvent *event)
{
	struct platform_keystroke *keystroke;
	BYTE flags = 0;

	if (keystroke_count == KEYSTROKE_QUEUE_SIZE)
	{
		keystroke_head = (keystroke_head + 1) % KEYSTROKE_QUEUE_SIZE;
		keystroke_count--;
	}
	keystroke = &keystroke_queue[(keystroke_head + keystroke_count) % KEYSTROKE_QUEUE_SIZE];
	if (event->mod & SDL_KMOD_CTRL) flags |= 0x01;
	if (event->mod & SDL_KMOD_SHIFT) flags |= 0x02;
	if (event->mod & SDL_KMOD_ALT) flags |= 0x04;
	if (event->mod & SDL_KMOD_CAPS) flags |= 0x08;
	if (event->mod & SDL_KMOD_NUM) flags |= 0x10;
	if (!event->down) flags |= 0x40;
	if (event->repeat) flags |= 0x80;
	keystroke->virtual_key = virtual_key_from_scancode(event->scancode);
	keystroke->ascii = event->down ? ascii_from_key(event->key, event->mod) : 0;
	keystroke->flags = flags;
	keystroke_count++;
}

BOOL platform_next_keystroke(struct platform_keystroke *keystroke)
{
	BOOL result = FALSE;

	pthread_mutex_lock(&input_lock);
	if (keystroke_count)
	{
		*keystroke = keystroke_queue[keystroke_head];
		keystroke_head = (keystroke_head + 1) % KEYSTROKE_QUEUE_SIZE;
		keystroke_count--;
		result = TRUE;
	}
	pthread_mutex_unlock(&input_lock);
	return result;
}

/* ---------- internet play's invite links (p2p.c) */

#ifdef HALO_ANDROID
/* SDL declares it for Android builds only, which the guest is not
(guest/runtime/guest_sdl.c passes it to the host) */
bool SDL_ShowAndroidToast(const char *message, int duration, int gravity, int xoffset, int yoffset);
#endif

/* whether the text has an invite link in it (its prefix, in any case) */
static BOOL platform_text_has_invite_link(const char *text)
{
	static const char prefix[] = "halo://join/";
	size_t length = sizeof(prefix) - 1;

	for (; *text; text++)
	{
		size_t index;

		for (index = 0; index < length && text[index] &&
			(text[index] | 0x20) == prefix[index]; index++)
		{
		}
		if (index == length)
			return TRUE;
	}
	return FALSE;
}

/* the clipboard's text (the menus' text fields' Ctrl+V), and text put on it
(the server settings' invite link); 0 if there is none. The main thread's */
int platform_clipboard_get(char *text, int size)
{
	char *clipboard = SDL_GetClipboardText();
	int got = clipboard && *clipboard;

	snprintf(text, (size_t)size, "%s", got ? clipboard : "");
	SDL_free(clipboard);
	return got;
}

void platform_clipboard_set(const char *text)
{
	SDL_SetClipboardText(text);
}

/* puts a new invite on the clipboard, and joins one found there when the
game comes to the front */
static void platform_invite_clipboard(BOOL look)
{
	/* the last clipboard text looked at, so each invite is joined once */
	static char seen[256];
	const char *invite = p2p_take_clipboard_text();

	if (invite)
	{
		SDL_SetClipboardText(invite);
		snprintf(seen, sizeof(seen), "%s", invite);
		platform_log("Internet play: the invite link is on the clipboard");
#ifdef HALO_ANDROID
		SDL_ShowAndroidToast("Hosting: the invite link is on the clipboard", 1, -1, 0, 0);
#endif
	}
	if (look && config_boolean("network.join_from_clipboard"))
	{
		char *text = SDL_GetClipboardText();

		if (text && strcmp(text, seen) && strlen(text) < sizeof(seen))
		{
			snprintf(seen, sizeof(seen), "%s", text);
			/* (a link, not a bare code: 64 hex digits alone are as often a
			checksum copied for something else) */
			if (platform_text_has_invite_link(text) && p2p_join_invite(text))
			{
#ifdef HALO_ANDROID
				SDL_ShowAndroidToast("Joining the invite on the clipboard", 1, -1, 0, 0);
#endif
			}
		}
		SDL_free(text);
	}
}

/* ---------- messages for the player */

/* a message waiting for the event pump to show it (on the window's thread,
between frames) */
static pthread_mutex_t platform_message_lock = PTHREAD_MUTEX_INITIALIZER;
static char platform_message_title[80];
static char platform_message_text[600];
static BOOL platform_message_pending;

/* shows the player a message in a box of its own (the network code's: a
host of another version), and logs it */
void platform_show_message(const char *title, const char *message)
{
	platform_log("%s: %s", title, message);
	/* (a run nobody watches: the log only) */
	if (config_boolean("debug.hidden_window") || config_boolean("debug.null_renderer"))
		return;
	pthread_mutex_lock(&platform_message_lock);
	snprintf(platform_message_title, sizeof(platform_message_title), "%s", title);
	snprintf(platform_message_text, sizeof(platform_message_text), "%s", message);
	platform_message_pending = TRUE;
	pthread_mutex_unlock(&platform_message_lock);
}

static void platform_show_pending_message(void)
{
	char title[sizeof(platform_message_title)];
	char text[sizeof(platform_message_text)];
	BOOL pending;

	pthread_mutex_lock(&platform_message_lock);
	pending = platform_message_pending;
	platform_message_pending = FALSE;
	memcpy(title, platform_message_title, sizeof(title));
	memcpy(text, platform_message_text, sizeof(text));
	pthread_mutex_unlock(&platform_message_lock);
	if (!pending)
		return;
#ifdef HALO_ANDROID
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, title, text, NULL);
#else
	{
		/* (a box cannot show above a fullscreen game) */
		int fullscreen = (SDL_GetWindowFlags(platform_window) & SDL_WINDOW_FULLSCREEN) != 0;

		if (fullscreen)
			SDL_SetWindowFullscreen(platform_window, false);
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, title, text, platform_window);
		if (fullscreen)
			SDL_SetWindowFullscreen(platform_window, true);
	}
#endif
}

/* ---------- events */

/* quits as closing the window does, when the events are next read (the
menus' Quit: port/linux/game/menu_functions.c); Android's menus have none,
as the system closes its apps */
void platform_request_quit(void)
{
#ifndef HALO_ANDROID
	SDL_Event event;

	memset(&event, 0, sizeof(event));
	event.type = SDL_EVENT_QUIT;
	SDL_PushEvent(&event);
#endif
}

void platform_scoreboard_scroll(int open, long *notches, long *pages)
{
	Uint64 now = SDL_GetTicks();

	pthread_mutex_lock(&input_lock);
	if (!open || now >= scoreboard_open_until_ms)
	{
		scoreboard_wheel = 0.0f;
		scoreboard_notches = 0;
		scoreboard_pages = 0;
	}
	scoreboard_open_until_ms = open ? now + SCOREBOARD_OPEN_MS : 0;
	if (notches)
		*notches = scoreboard_notches;
	if (pages)
		*pages = scoreboard_pages;
	scoreboard_notches = 0;
	scoreboard_pages = 0;
	pthread_mutex_unlock(&input_lock);
}

void platform_pump_events(void)
{
	/* debug.exit_after (seconds) ends the game that long after the window
	opens, as closing it does (tools/pgo_train.py) */
	static Uint64 exit_ticks = (Uint64)-1;
	SDL_Event event;
	static BOOL looked_at_clipboard;
	BOOL look_at_clipboard = !looked_at_clipboard;

	if (!platform_window || SDL_GetCurrentThreadID() != platform_event_thread)
		return;
	if (exit_ticks == (Uint64)-1)
	{
		double seconds = config_real("debug.exit_after");

		exit_ticks = seconds > 0.0 ? SDL_GetTicks() + (Uint64)(seconds * 1000.0) : 0;
	}
	if (exit_ticks && SDL_GetTicks() >= exit_ticks)
	{
		platform_log("exiting after debug.exit_after");
		exit(EXIT_SUCCESS);
	}
	platform_show_pending_message();
#ifndef HALO_ANDROID
	updater_poll(platform_window);
#endif
	pthread_mutex_lock(&input_lock);
	while (SDL_PollEvent(&event))
	{
		switch (event.type)
		{
		case SDL_EVENT_QUIT:
			pthread_mutex_unlock(&input_lock);
			platform_log("window closed");
			exit(EXIT_SUCCESS);
		case SDL_EVENT_KEY_DOWN:
		case SDL_EVENT_KEY_UP:
			if (event.key.scancode < SDL_SCANCODE_COUNT)
			{
				input_state.keys[event.key.scancode] = event.key.down;
				if (event.key.down)
					keys_pressed[event.key.scancode] = 1;
			}
			if (binding_capture == _binding_capture_waiting && event.key.down && !event.key.repeat &&
				event.key.scancode != SDL_SCANCODE_F11 && event.key.scancode != SDL_SCANCODE_F12)
			{
				binding_capture = _binding_capture_taken;
				binding_taken_ms = SDL_GetTicks();
				binding_capture_result = event.key.scancode == SDL_SCANCODE_ESCAPE ? 3 :
					event.key.scancode == SDL_SCANCODE_DELETE ? 2 : 1;
				binding_captured_input = event.key.scancode;
				break;
			}
			queue_keystroke(&event.key);
			if (SDL_GetTicks() < scoreboard_open_until_ms && event.key.down &&
				(event.key.scancode == SDL_SCANCODE_PAGEUP || event.key.scancode == SDL_SCANCODE_PAGEDOWN))
			{
				scoreboard_pages += event.key.scancode == SDL_SCANCODE_PAGEDOWN ? 1 : -1;
			}
			/* F12 releases or recaptures the mouse */
			if (event.key.down && !event.key.repeat && event.key.scancode == SDL_SCANCODE_F12)
			{
				input_state.mouse_released = !input_state.mouse_released;
				platform_mouse_capture(!input_state.mouse_released && !input_state.ui_pointer);
			}
#ifndef HALO_ANDROID
			/* F11 switches between fullscreen and the window (SDL keeps the
			window's size and place while fullscreen) */
			if (event.key.down && !event.key.repeat && event.key.scancode == SDL_SCANCODE_F11)
			{
				SDL_SetWindowFullscreen(platform_window,
					(SDL_GetWindowFlags(platform_window) & SDL_WINDOW_FULLSCREEN) ? false : true);
			}
#endif
			break;
		case SDL_EVENT_MOUSE_MOTION:
#ifndef HALO_ANDROID
			/* in the menus the mouse moves the pointer, not the view */
			if (input_state.ui_pointer)
			{
				ui_pointer.x = event.motion.x;
				ui_pointer.y = event.motion.y;
				ui_pointer.moved = TRUE;
				break;
			}
#endif
			input_state.mouse_dx += event.motion.xrel;
			input_state.mouse_dy += event.motion.yrel;
			break;
		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_BUTTON_UP:
			if (event.button.button < 32)
			{
				if (event.button.down)
					mouse_buttons_down |= 1u << event.button.button;
				else
					mouse_buttons_down &= ~(1u << event.button.button);
			}
			if (binding_capture == _binding_capture_waiting && event.button.down &&
				event.button.button < PLATFORM_MOUSE_BUTTON_COUNT)
			{
				binding_capture = _binding_capture_taken;
				binding_taken_ms = SDL_GetTicks();
				binding_capture_result = 1;
				binding_captured_input = INPUT_MOUSE + event.button.button;
				break;
			}
#ifndef HALO_ANDROID
			/* clicks in the menus go to the pointer; a button held down
			when the menu closes stays up until pressed again, so the click
			that resumes the game does not also fire */
			if (input_state.ui_pointer)
			{
				if (event.button.down && event.button.button == SDL_BUTTON_LEFT)
				{
					ui_pointer.left_clicks++;
					ui_pointer.click_x = event.button.x;
					ui_pointer.click_y = event.button.y;
				}
				else if (event.button.down && event.button.button == SDL_BUTTON_RIGHT)
				{
					ui_pointer.right_clicks++;
				}
				break;
			}
#endif
			if (event.button.button < PLATFORM_MOUSE_BUTTON_COUNT)
			{
				input_state.mouse_buttons[event.button.button] = event.button.down;
				if (event.button.down)
					mouse_buttons_pressed[event.button.button] = 1;
			}
			break;
		case SDL_EVENT_MOUSE_WHEEL:
			if (binding_capture == _binding_capture_waiting && event.wheel.y != 0.0f)
			{
				binding_capture = _binding_capture_taken;
				binding_taken_ms = SDL_GetTicks();
				binding_capture_result = 1;
				binding_captured_input = event.wheel.y > 0.0f ? INPUT_WHEEL_UP : INPUT_WHEEL_DOWN;
				break;
			}
			if (SDL_GetTicks() < scoreboard_open_until_ms)
			{
				/* whole notches, up (away) scrolling up */
				scoreboard_wheel -= event.wheel.y;
				while (scoreboard_wheel >= 1.0f)
				{
					scoreboard_notches++;
					scoreboard_wheel -= 1.0f;
				}
				while (scoreboard_wheel <= -1.0f)
				{
					scoreboard_notches--;
					scoreboard_wheel += 1.0f;
				}
				break;
			}
#ifndef HALO_ANDROID
			if (input_state.ui_pointer)
			{
				/* whole notches: smooth-scrolling wheels send fractions */
				ui_pointer_wheel += event.wheel.y;
				while (ui_pointer_wheel >= 1.0f)
				{
					ui_pointer.wheel_steps++;
					ui_pointer_wheel -= 1.0f;
				}
				while (ui_pointer_wheel <= -1.0f)
				{
					ui_pointer.wheel_steps--;
					ui_pointer_wheel += 1.0f;
				}
				break;
			}
#endif
			input_state.mouse_wheel += event.wheel.y;
			break;
		case SDL_EVENT_WINDOW_FOCUS_LOST:
			memset(input_state.keys, 0, sizeof(input_state.keys));
			memset(input_state.mouse_buttons, 0, sizeof(input_state.mouse_buttons));
			memset(mouse_buttons_pressed, 0, sizeof(mouse_buttons_pressed));
			input_state.focused = FALSE;
			break;
		case SDL_EVENT_WINDOW_FOCUS_GAINED:
			input_state.focused = TRUE;
			look_at_clipboard = TRUE;
#ifndef HALO_ANDROID
			if (!input_state.mouse_released && !input_state.ui_pointer)
				platform_mouse_capture(TRUE);
#endif
			break;
		case SDL_EVENT_GAMEPAD_ADDED:
			SDL_OpenGamepad(event.gdevice.which);
			break;
		default:
			break;
		}
	}
	pthread_mutex_unlock(&input_lock);
	looked_at_clipboard = TRUE;
	platform_invite_clipboard(look_at_clipboard);
}

void platform_menus_set_active(BOOL active)
{
	pthread_mutex_lock(&input_lock);
	input_state.menus = active;
	pthread_mutex_unlock(&input_lock);
}

void platform_binding_capture_begin(void)
{
	pthread_mutex_lock(&input_lock);
	binding_capture = _binding_capture_waiting;
	binding_settling = TRUE;
	binding_polled_ms = SDL_GetTicks();
	pthread_mutex_unlock(&input_lock);
}

int platform_binding_capture_poll(int *input)
{
	int result = 0;

	pthread_mutex_lock(&input_lock);
	binding_polled_ms = SDL_GetTicks();
	if (binding_capture == _binding_capture_taken)
	{
		result = binding_capture_result;
		*input = binding_captured_input;
		binding_capture = _binding_capture_idle;
	}
	pthread_mutex_unlock(&input_lock);
	return result;
}

#ifndef HALO_ANDROID
/* ---------- the menus' pointer */

/* While a menu is up the mouse is released, its pointer shows (centered when
the menu opens) and its motion, clicks and wheel go to the menus
(halo_ui_pointer_update, d3d8_gl.c) instead of the controller and the aim. */
void platform_ui_pointer_set_active(BOOL active)
{
	if (!platform_window || (active != FALSE) == (input_state.ui_pointer != FALSE))
		return;
	pthread_mutex_lock(&input_lock);
	input_state.ui_pointer = active;
	memset(&ui_pointer, 0, sizeof(ui_pointer));
	ui_pointer_wheel = 0.0f;
	input_state.mouse_dx = 0.0f;
	input_state.mouse_dy = 0.0f;
	input_state.mouse_wheel = 0.0f;
	memset(input_state.mouse_buttons, 0, sizeof(input_state.mouse_buttons));
	memset(mouse_buttons_pressed, 0, sizeof(mouse_buttons_pressed));
	pthread_mutex_unlock(&input_lock);
	platform_mouse_capture(!active && !input_state.mouse_released);
	if (active)
	{
		int width, height;

		SDL_GetWindowSize(platform_window, &width, &height);
		SDL_WarpMouseInWindow(platform_window, width * 0.5f, height * 0.5f);
		pthread_mutex_lock(&input_lock);
		ui_pointer.x = width * 0.5f;
		ui_pointer.y = height * 0.5f;
		pthread_mutex_unlock(&input_lock);
	}
}

/* what the pointer did since the last call; FALSE when it is not active */
BOOL platform_ui_pointer_read(struct platform_ui_pointer *pointer)
{
	BOOL active;

	pthread_mutex_lock(&input_lock);
	active = input_state.ui_pointer;
	*pointer = ui_pointer;
	ui_pointer.moved = FALSE;
	ui_pointer.left_clicks = 0;
	ui_pointer.right_clicks = 0;
	ui_pointer.wheel_steps = 0;
	pthread_mutex_unlock(&input_lock);
	return active;
}

void platform_video_window_size(int *width, int *height)
{
	SDL_GetWindowSize(platform_window, width, height);
}

#endif
void platform_input_read(struct platform_input_state *state, BOOL consume_motion)
{
	pthread_mutex_lock(&input_lock);
	*state = input_state;
	/* (rebinding: nothing reaches the controller until the input is taken
	and every key and button is up again) */
	if (binding_capture != _binding_capture_idle || binding_settling)
	{
		int scancode;
		BOOL held = mouse_buttons_down != 0;

		for (scancode = 0; scancode < SDL_SCANCODE_COUNT && !held; scancode++)
			held = input_state.keys[scancode] != 0;
		if (binding_capture == _binding_capture_taken && SDL_GetTicks() - binding_taken_ms > BINDING_UNCLAIMED_MS)
			binding_capture = _binding_capture_idle;
		if (binding_capture == _binding_capture_waiting && SDL_GetTicks() - binding_polled_ms > BINDING_ABANDONED_MS)
			binding_capture = _binding_capture_idle;
		if (binding_capture == _binding_capture_idle && !held)
			binding_settling = FALSE;
		memset(state->keys, 0, sizeof(state->keys));
		memset(state->mouse_buttons, 0, sizeof(state->mouse_buttons));
		memset(keys_pressed, 0, sizeof(keys_pressed));
		memset(mouse_buttons_pressed, 0, sizeof(mouse_buttons_pressed));
		state->mouse_wheel = 0.0f;
		/* (and the motion of the while, which would otherwise pile up for
		the aim) */
		state->mouse_dx = state->mouse_dy = 0.0f;
		if (consume_motion)
		{
			input_state.mouse_dx = input_state.mouse_dy = 0.0f;
			input_state.mouse_wheel = 0.0f;
		}
		pthread_mutex_unlock(&input_lock);
		return;
	}
	if (consume_motion)
	{
		int scancode;

		for (scancode = 0; scancode < SDL_SCANCODE_COUNT; scancode++)
		{
			state->keys[scancode] |= keys_pressed[scancode];
			keys_pressed[scancode] = 0;
		}
		for (scancode = 0; scancode < PLATFORM_MOUSE_BUTTON_COUNT; scancode++)
		{
			state->mouse_buttons[scancode] |= mouse_buttons_pressed[scancode];
			mouse_buttons_pressed[scancode] = 0;
		}
	}
	if (consume_motion)
	{
		input_state.mouse_dx = 0;
		input_state.mouse_dy = 0;
		input_state.mouse_wheel = 0;
	}
	pthread_mutex_unlock(&input_lock);
}
