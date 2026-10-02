/*
WEB_SDL.C

The SDL3 functions the platform layer calls (sdl_platform.c, xinput_sdl.c,
dsound_sdl.c, gl_functions.c), over the browser. The list is the Android
guest's (port/android/guest/runtime/guest_sdl.c).

The game runs on a pthread, in a Web Worker, so it may block like a native
program. What only the page's main thread can reach (the DOM's events, the
Gamepad API, Web Audio) arrives through the shared state (web_shared.h),
which port/web/site/app.js fills. The OpenGL ES 3 context is WebGL 2 on an
OffscreenCanvas that belongs to the game's thread (web_library.js); each
frame goes to the page as an ImageBitmap.
*/

#include <emscripten.h>
#include <emscripten/html5_webgl.h>
#include <emscripten/threading.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <SDL3/SDL.h>
#include <GLES3/gl3.h>

#include "web_shared.h"

/* web_library.js */
int web_js_gl_create(int width, int height, int batch_streams, int present_ack, int pixel_frames);
void web_js_gl_resize(int width, int height);
void web_js_gl_flush(void);
void web_js_gl_present(void);
void web_js_post(int kind, const char *text);
int web_quick_play_background_active(void);

/* ---------- the shared state */

static struct web_shared_state shared_state = { WEB_SHARED_MAGIC, WEB_SHARED_VERSION, sizeof(struct web_shared_state) };

EMSCRIPTEN_KEEPALIVE struct web_shared_state *web_shared_state(void)
{
	return &shared_state;
}

/* the byte offsets of the members the page uses, so that app.js need not
repeat the structure's layout */
enum
{
	WEB_OFFSET_SIZE,
	WEB_OFFSET_EVENT_WRITE,
	WEB_OFFSET_EVENT_READ,
	WEB_OFFSET_EVENTS,
	WEB_OFFSET_EVENT_SIZE,
	WEB_OFFSET_GAMEPADS,
	WEB_OFFSET_GAMEPAD_SIZE,
	WEB_OFFSET_DISPLAY_WIDTH,
	WEB_OFFSET_DISPLAY_HEIGHT,
	WEB_OFFSET_FRAME_COUNTER,
	WEB_OFFSET_FRAMES_PRESENTED,
	WEB_OFFSET_VSYNC,
	WEB_OFFSET_AUDIO_RATE,
	WEB_OFFSET_AUDIO_OPEN,
	WEB_OFFSET_AUDIO_WRITE,
	WEB_OFFSET_AUDIO_READ,
	WEB_OFFSET_AUDIO_UNDERRUNS,
	WEB_OFFSET_AUDIO_RING,
	WEB_OFFSET_AUDIO_RING_FRAMES,
	WEB_OFFSET_PAGE_HIDDEN,
	WEB_OFFSET_GAME_STARTED,
	WEB_OFFSET_EVENT_CAPACITY,
	WEB_OFFSET_GAMEPAD_COUNT,
	WEB_OFFSET_NET_LOCAL_ADDRESS,
	WEB_OFFSET_NET_OUT_WRITE,
	WEB_OFFSET_NET_OUT_READ,
	WEB_OFFSET_NET_IN_WRITE,
	WEB_OFFSET_NET_IN_READ,
	WEB_OFFSET_NET_OUT,
	WEB_OFFSET_NET_OUT_BYTES,
	WEB_OFFSET_NET_IN,
	WEB_OFFSET_NET_IN_BYTES,
	WEB_OFFSET_GATEWAY_ENABLED,
	WEB_OFFSET_GATEWAY_IDENTIFIER,
	WEB_OFFSET_GATEWAY_PEERS,
	WEB_OFFSET_GATEWAY_PEER_COUNT,
	WEB_OFFSET_PING_SEQUENCE,
	WEB_OFFSET_PING_HOST,
	WEB_OFFSET_PING_EPOCH,
	WEB_OFFSET_PING_UPDATED,
	WEB_OFFSET_PING_COUNT,
	WEB_OFFSET_PING_PEERS,
	WEB_OFFSET_PING_PEER_COUNT,
	NUMBER_OF_WEB_OFFSETS
};

EMSCRIPTEN_KEEPALIVE const int *web_shared_offsets(void)
{
	static int offsets[NUMBER_OF_WEB_OFFSETS];

#define OFFSET(member) ((int)((char *)&shared_state.member - (char *)&shared_state))
	offsets[WEB_OFFSET_SIZE] = (int)sizeof(shared_state);
	offsets[WEB_OFFSET_EVENT_WRITE] = OFFSET(event_write);
	offsets[WEB_OFFSET_EVENT_READ] = OFFSET(event_read);
	offsets[WEB_OFFSET_EVENTS] = OFFSET(events);
	offsets[WEB_OFFSET_EVENT_SIZE] = (int)sizeof(struct web_event);
	offsets[WEB_OFFSET_GAMEPADS] = OFFSET(gamepads);
	offsets[WEB_OFFSET_GAMEPAD_SIZE] = (int)sizeof(struct web_gamepad);
	offsets[WEB_OFFSET_DISPLAY_WIDTH] = OFFSET(display_width);
	offsets[WEB_OFFSET_DISPLAY_HEIGHT] = OFFSET(display_height);
	offsets[WEB_OFFSET_FRAME_COUNTER] = OFFSET(frame_counter);
	offsets[WEB_OFFSET_FRAMES_PRESENTED] = OFFSET(frames_presented);
	offsets[WEB_OFFSET_VSYNC] = OFFSET(vsync);
	offsets[WEB_OFFSET_AUDIO_RATE] = OFFSET(audio_rate);
	offsets[WEB_OFFSET_AUDIO_OPEN] = OFFSET(audio_open);
	offsets[WEB_OFFSET_AUDIO_WRITE] = OFFSET(audio_write);
	offsets[WEB_OFFSET_AUDIO_READ] = OFFSET(audio_read);
	offsets[WEB_OFFSET_AUDIO_UNDERRUNS] = OFFSET(audio_underruns);
	offsets[WEB_OFFSET_AUDIO_RING] = OFFSET(audio_ring);
	offsets[WEB_OFFSET_AUDIO_RING_FRAMES] = WEB_AUDIO_RING_FRAMES;
	offsets[WEB_OFFSET_PAGE_HIDDEN] = OFFSET(page_hidden);
	offsets[WEB_OFFSET_GAME_STARTED] = OFFSET(game_started);
	offsets[WEB_OFFSET_EVENT_CAPACITY] = WEB_EVENT_CAPACITY;
	offsets[WEB_OFFSET_GAMEPAD_COUNT] = WEB_GAMEPAD_COUNT;
	offsets[WEB_OFFSET_NET_LOCAL_ADDRESS] = OFFSET(net_local_address);
	offsets[WEB_OFFSET_NET_OUT_WRITE] = OFFSET(net_out_write);
	offsets[WEB_OFFSET_NET_OUT_READ] = OFFSET(net_out_read);
	offsets[WEB_OFFSET_NET_IN_WRITE] = OFFSET(net_in_write);
	offsets[WEB_OFFSET_NET_IN_READ] = OFFSET(net_in_read);
	offsets[WEB_OFFSET_NET_OUT] = OFFSET(net_out);
	offsets[WEB_OFFSET_NET_OUT_BYTES] = WEB_NET_OUT_BYTES;
	offsets[WEB_OFFSET_NET_IN] = OFFSET(net_in);
	offsets[WEB_OFFSET_NET_IN_BYTES] = WEB_NET_IN_BYTES;
	offsets[WEB_OFFSET_GATEWAY_ENABLED] = OFFSET(gateway_enabled);
	offsets[WEB_OFFSET_GATEWAY_IDENTIFIER] = OFFSET(gateway_identifier);
	offsets[WEB_OFFSET_GATEWAY_PEERS] = OFFSET(gateway_peers);
	offsets[WEB_OFFSET_GATEWAY_PEER_COUNT] = WEB_GATEWAY_PEERS;
	offsets[WEB_OFFSET_PING_SEQUENCE] = OFFSET(ping_sequence);
	offsets[WEB_OFFSET_PING_HOST] = OFFSET(ping_host);
	offsets[WEB_OFFSET_PING_EPOCH] = OFFSET(ping_epoch);
	offsets[WEB_OFFSET_PING_UPDATED] = OFFSET(ping_updated);
	offsets[WEB_OFFSET_PING_COUNT] = OFFSET(ping_count);
	offsets[WEB_OFFSET_PING_PEERS] = OFFSET(ping_peers);
	offsets[WEB_OFFSET_PING_PEER_COUNT] = WEB_PING_PEERS;
#undef OFFSET
	return offsets;
}

/* ---------- basics */

static char sdl_error[256];

static void set_error(const char *text)
{
	snprintf(sdl_error, sizeof(sdl_error), "%s", text);
}

bool SDL_Init(SDL_InitFlags flags)
{
	(void)flags;
	return true;
}

bool SDL_SetHint(const char *name, const char *value)
{
	(void)name;
	(void)value;
	return true;
}

const char *SDL_GetError(void)
{
	return sdl_error;
}

Uint64 SDL_GetTicks(void)
{
	return (Uint64)emscripten_get_now();
}

SDL_ThreadID SDL_GetCurrentThreadID(void)
{
	return (SDL_ThreadID)(uintptr_t)pthread_self();
}

void SDL_free(void *memory)
{
	free(memory);
}

void SDL_Delay(Uint32 milliseconds)
{
	usleep((useconds_t)milliseconds * 1000);
}

/* ---------- the clipboard, messages */

static char clipboard[1024];

bool SDL_SetClipboardText(const char *text)
{
	snprintf(clipboard, sizeof(clipboard), "%s", text ? text : "");
	web_js_post(2, clipboard);
	return true;
}

char *SDL_GetClipboardText(void)
{
	return strdup(clipboard);
}

bool SDL_ShowAndroidToast(const char *message, int duration, int gravity, int xoffset, int yoffset)
{
	(void)duration;
	(void)gravity;
	(void)xoffset;
	(void)yoffset;
	web_js_post(1, message);
	return true;
}

bool SDL_ShowSimpleMessageBox(SDL_MessageBoxFlags flags, const char *title, const char *message, SDL_Window *window)
{
	char text[1024];

	(void)flags;
	(void)window;
	snprintf(text, sizeof(text), "%s: %s", title ? title : "Halo", message ? message : "");
	web_js_post(1, text);
	return true;
}

/* ---------- video */

#define WEB_WINDOW ((SDL_Window *)1)

static int gl_context;
static int canvas_width, canvas_height;
static int swap_interval = 1;
static int last_frame_counter;

static void display_size(int *width, int *height)
{
	int w = shared_state.display_width;
	int h = shared_state.display_height;

	if (w < 64 || h < 64)
	{
		w = 1280;
		h = 720;
	}
	*width = w;
	*height = h;
}

SDL_Window *SDL_CreateWindow(const char *title, int width, int height, SDL_WindowFlags flags)
{
	(void)title;
	(void)width;
	(void)height;
	(void)flags;
	return WEB_WINDOW;
}

bool SDL_GetWindowSizeInPixels(SDL_Window *window, int *width, int *height)
{
	(void)window;
	if (width)
		*width = canvas_width;
	if (height)
		*height = canvas_height;
	return true;
}

bool SDL_SetWindowRelativeMouseMode(SDL_Window *window, bool enabled)
{
	(void)window;
	(void)enabled;
	/* the page locks the pointer when the player clicks the game */
	return true;
}

bool SDL_GL_SetAttribute(SDL_GLAttr attribute, int value)
{
	(void)attribute;
	(void)value;
	return true;
}

SDL_GLContext SDL_GL_CreateContext(SDL_Window *window)
{
	(void)window;
	if (!gl_context)
	{
		const char *batch_streams = getenv("HALO_WEB_BATCH_STREAMS");
		const char *geometry_cache = getenv("HALO_WEB_GEOMETRY_CACHE");
		const char *present_ack = getenv("HALO_WEB_PRESENT_ACK");
		const char *pixel_frames = getenv("HALO_WEB_PIXEL_FRAMES");

		display_size(&canvas_width, &canvas_height);
		gl_context = web_js_gl_create(canvas_width, canvas_height,
			(!batch_streams || strcmp(batch_streams, "0")) &&
			!(geometry_cache && !strcmp(geometry_cache, "1")),
			present_ack && !strcmp(present_ack, "1"),
			pixel_frames && !strcmp(pixel_frames, "1"));
		if (!gl_context)
		{
			set_error("WebGL 2 is not available");
			return NULL;
		}
		shared_state.game_started = 1;
	}
	return (SDL_GLContext)(uintptr_t)gl_context;
}

bool SDL_GL_MakeCurrent(SDL_Window *window, SDL_GLContext context)
{
	(void)window;
	return context && emscripten_webgl_make_context_current((EMSCRIPTEN_WEBGL_CONTEXT_HANDLE)(uintptr_t)context) ==
		EMSCRIPTEN_RESULT_SUCCESS;
}

bool SDL_GL_SetSwapInterval(int interval)
{
	swap_interval = interval;
	shared_state.vsync = interval != 0;
	return true;
}

bool SDL_GL_SwapWindow(SDL_Window *window)
{
	int width, height;
	static double last_swap_time;

	(void)window;
	if (!gl_context)
		return false;
	/* Hidden multiplayer still simulates, but never queues ImageBitmaps for
	a page whose rendering callbacks are suspended. */
	if (!shared_state.page_hidden)
	{
		web_js_gl_present();
		__atomic_add_fetch(&shared_state.frames_presented, 1, __ATOMIC_SEQ_CST);
	}
	else
		web_js_gl_flush();

	/* the next frame waits for the page's next animation frame, as a swap
	interval of one waits for the display. Multiplayer keeps a bounded 30Hz
	worker cadence while hidden; requestAnimationFrame stops in hidden tabs. */
	if (shared_state.page_hidden && web_quick_play_background_active())
	{
		double now = emscripten_get_now();
		double deadline = last_swap_time + 1000.0 / 30.0;
		while (shared_state.page_hidden && web_quick_play_background_active() &&
			now < deadline && deadline - now <= 1000.0 / 30.0)
		{
			int counter = __atomic_load_n(&shared_state.frame_counter, __ATOMIC_SEQ_CST);
			emscripten_futex_wait((void *)&shared_state.frame_counter, (uint32_t)counter, deadline - now);
			now = emscripten_get_now();
		}
	}
	else if (swap_interval || shared_state.page_hidden)
	{
		int counter = __atomic_load_n(&shared_state.frame_counter, __ATOMIC_SEQ_CST);

		while (counter == last_frame_counter)
		{
			emscripten_futex_wait((void *)&shared_state.frame_counter, (uint32_t)counter,
				shared_state.page_hidden ? 250.0 : 100.0);
			counter = __atomic_load_n(&shared_state.frame_counter, __ATOMIC_SEQ_CST);
			if (!shared_state.page_hidden || web_quick_play_background_active())
				break;
		}
		last_frame_counter = counter;
	}
	last_swap_time = emscripten_get_now();

	/* the page's canvas changed size (rotation, resizing): so does the
	drawing buffer, between frames */
	display_size(&width, &height);
	if (width != canvas_width || height != canvas_height)
	{
		canvas_width = width;
		canvas_height = height;
		web_js_gl_resize(width, height);
	}
	return true;
}

/* OpenGL ES 3.2 functions WebGL 2 lacks: the renderer asks for them, but
calls them only when the context has them (d3d8_gl.c gl_initialize) */
static void web_glCopyImageSubData(GLuint source, GLenum source_target, GLint source_level, GLint source_x,
	GLint source_y, GLint source_z, GLuint destination, GLenum destination_target, GLint destination_level,
	GLint destination_x, GLint destination_y, GLint destination_z, GLsizei width, GLsizei height, GLsizei depth)
{
}

static void web_glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void *indices,
	GLint base_vertex)
{
}

SDL_FunctionPointer SDL_GL_GetProcAddress(const char *name)
{
	void *function = emscripten_webgl_get_proc_address(name);

	if (!function && !strcmp(name, "glCopyImageSubData"))
		function = (void *)web_glCopyImageSubData;
	if (!function && !strcmp(name, "glDrawElementsBaseVertex"))
		function = (void *)web_glDrawElementsBaseVertex;
	return (SDL_FunctionPointer)function;
}

/* ---------- events */

bool SDL_PollEvent(SDL_Event *event)
{
	int read = __atomic_load_n(&shared_state.event_read, __ATOMIC_SEQ_CST);
	int write = __atomic_load_n(&shared_state.event_write, __ATOMIC_SEQ_CST);
	struct web_event source;

	if (read == write)
		return false;
	source = shared_state.events[read % WEB_EVENT_CAPACITY];
	__atomic_store_n(&shared_state.event_read, read + 1, __ATOMIC_SEQ_CST);
	if (!event)
		return true;
	memset(event, 0, sizeof(*event));
	event->common.timestamp = (Uint64)(emscripten_get_now() * 1000000.0);
	switch (source.type)
	{
	case WEB_EVENT_KEY:
		event->type = source.b ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
		event->key.scancode = (SDL_Scancode)source.a;
		event->key.down = source.b != 0;
		event->key.repeat = source.c != 0;
		event->key.key = (SDL_Keycode)source.d;
		event->key.mod = (SDL_Keymod)source.reserved;
		break;
	case WEB_EVENT_MOUSE_MOTION:
		event->type = SDL_EVENT_MOUSE_MOTION;
		event->motion.xrel = source.x;
		event->motion.yrel = source.y;
		break;
	case WEB_EVENT_MOUSE_BUTTON:
		event->type = source.b ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
		event->button.button = (Uint8)source.a;
		event->button.down = source.b != 0;
		event->button.clicks = 1;
		break;
	case WEB_EVENT_MOUSE_WHEEL:
		event->type = SDL_EVENT_MOUSE_WHEEL;
		event->wheel.y = source.y;
		break;
	case WEB_EVENT_FOCUS:
		event->type = source.a ? SDL_EVENT_WINDOW_FOCUS_GAINED : SDL_EVENT_WINDOW_FOCUS_LOST;
		break;
	case WEB_EVENT_GAMEPAD_ADDED:
		event->type = SDL_EVENT_GAMEPAD_ADDED;
		event->gdevice.which = (SDL_JoystickID)source.a;
		break;
	case WEB_EVENT_QUIT:
		/* the page never closes the game; a hidden page only pauses it */
		event->type = SDL_EVENT_USER;
		break;
	default:
		event->type = SDL_EVENT_USER;
		break;
	}
	return true;
}

/* ---------- gamepads: a gamepad handle is its slot plus one */

static struct web_gamepad *gamepad_slot(SDL_Gamepad *gamepad)
{
	uintptr_t index = (uintptr_t)gamepad;

	if (index < 1 || index > WEB_GAMEPAD_COUNT)
		return NULL;
	return &shared_state.gamepads[index - 1];
}

SDL_JoystickID *SDL_GetGamepads(int *count)
{
	SDL_JoystickID *result = malloc((WEB_GAMEPAD_COUNT + 1) * sizeof(SDL_JoystickID));
	int index, found = 0;

	if (!result)
		return NULL;
	for (index = 0; index < WEB_GAMEPAD_COUNT; index++)
	{
		if (shared_state.gamepads[index].connected && shared_state.gamepads[index].id)
			result[found++] = (SDL_JoystickID)shared_state.gamepads[index].id;
	}
	result[found] = 0;
	if (count)
		*count = found;
	return result;
}

SDL_Gamepad *SDL_GetGamepadFromID(SDL_JoystickID id)
{
	int index;

	for (index = 0; index < WEB_GAMEPAD_COUNT; index++)
	{
		if (shared_state.gamepads[index].connected && (SDL_JoystickID)shared_state.gamepads[index].id == id)
			return (SDL_Gamepad *)(uintptr_t)(index + 1);
	}
	return NULL;
}

SDL_Gamepad *SDL_OpenGamepad(SDL_JoystickID id)
{
	return SDL_GetGamepadFromID(id);
}

Sint16 SDL_GetGamepadAxis(SDL_Gamepad *gamepad, SDL_GamepadAxis axis)
{
	struct web_gamepad *pad = gamepad_slot(gamepad);
	int value;

	if (!pad || !pad->connected || axis < 0 || axis >= 6)
		return 0;
	value = pad->axes[axis];
	if (value < -32768)
		value = -32768;
	if (value > 32767)
		value = 32767;
	return (Sint16)value;
}

bool SDL_GetGamepadButton(SDL_Gamepad *gamepad, SDL_GamepadButton button)
{
	struct web_gamepad *pad = gamepad_slot(gamepad);

	if (!pad || !pad->connected || button < 0 || button >= 32)
		return false;
	return (pad->buttons & (1u << button)) != 0;
}

SDL_GamepadType SDL_GetGamepadType(SDL_Gamepad *gamepad)
{
	struct web_gamepad *pad = gamepad_slot(gamepad);

	return pad && pad->connected ? (SDL_GamepadType)pad->type : SDL_GAMEPAD_TYPE_UNKNOWN;
}

bool SDL_RumbleGamepad(SDL_Gamepad *gamepad, Uint16 low, Uint16 high, Uint32 milliseconds)
{
	struct web_gamepad *pad = gamepad_slot(gamepad);

	if (!pad || !pad->connected)
		return false;
	if (pad->rumble_low == low && pad->rumble_high == high)
		return true;
	pad->rumble_low = low;
	pad->rumble_high = high;
	pad->rumble_milliseconds = (int32_t)milliseconds;
	__atomic_add_fetch(&pad->rumble_serial, 1, __ATOMIC_SEQ_CST);
	return true;
}

/* ---------- audio

A thread of its own keeps the ring about AUDIO_TARGET_FRAMES ahead of the
page's AudioWorklet, calling the mixer (dsound_sdl.c's callback) as SDL's
audio thread would. Until the page's audio runs (browsers start it only
after the player touches the page) and while the page is hidden, the ring is
drained in real time instead, so the game's sounds still finish on time. */

#define AUDIO_TARGET_FRAMES 3072
#define AUDIO_CHANNELS 2

struct web_audio_stream
{
	SDL_AudioStreamCallback callback;
	void *userdata;
	int rate;
	volatile int resumed;
};

static struct web_audio_stream audio_stream;
static pthread_t audio_thread;

static void *audio_thread_main(void *parameter)
{
	double drained_at = emscripten_get_now();

	(void)parameter;
	for (;;)
	{
		int write = __atomic_load_n(&shared_state.audio_write, __ATOMIC_SEQ_CST);
		int read = __atomic_load_n(&shared_state.audio_read, __ATOMIC_SEQ_CST);
		int queued = write - read;

		if (!shared_state.audio_open || shared_state.page_hidden)
		{
			/* nobody plays the ring: consume it at the sample rate */
			double now = emscripten_get_now();
			int frames = (int)((now - drained_at) * audio_stream.rate / 1000.0);

			if (frames > 0)
			{
				drained_at += frames * 1000.0 / audio_stream.rate;
				if (frames > queued)
					frames = queued;
				__atomic_add_fetch(&shared_state.audio_read, frames, __ATOMIC_SEQ_CST);
				queued -= frames;
			}
		}
		else
		{
			drained_at = emscripten_get_now();
		}
		if (queued < 0)
		{
			/* the worklet ran ahead of the ring (an underrun) */
			__atomic_store_n(&shared_state.audio_read, write, __ATOMIC_SEQ_CST);
			queued = 0;
		}
		if (audio_stream.resumed && queued < AUDIO_TARGET_FRAMES)
		{
			int wanted = AUDIO_TARGET_FRAMES - queued;

			audio_stream.callback(audio_stream.userdata, (SDL_AudioStream *)&audio_stream,
				wanted * AUDIO_CHANNELS * (int)sizeof(float), wanted * AUDIO_CHANNELS * (int)sizeof(float));
		}
		usleep(4000);
	}
	return NULL;
}

SDL_AudioStream *SDL_OpenAudioDeviceStream(SDL_AudioDeviceID device, const SDL_AudioSpec *spec,
	SDL_AudioStreamCallback callback, void *userdata)
{
	(void)device;
	if (!spec || spec->format != SDL_AUDIO_F32 || spec->channels != AUDIO_CHANNELS || !callback)
	{
		set_error("unsupported audio format");
		return NULL;
	}
	if (audio_stream.callback)
	{
		set_error("the audio device is open");
		return NULL;
	}
	audio_stream.callback = callback;
	audio_stream.userdata = userdata;
	audio_stream.rate = spec->freq;
	shared_state.audio_rate = spec->freq;
	if (pthread_create(&audio_thread, NULL, audio_thread_main, NULL) != 0)
	{
		audio_stream.callback = NULL;
		set_error("cannot start the audio thread");
		return NULL;
	}
	pthread_detach(audio_thread);
	return (SDL_AudioStream *)&audio_stream;
}

bool SDL_PutAudioStreamData(SDL_AudioStream *stream, const void *data, int length)
{
	const float *samples = data;
	int frames = length / (AUDIO_CHANNELS * (int)sizeof(float));
	int write = __atomic_load_n(&shared_state.audio_write, __ATOMIC_SEQ_CST);
	int index;

	(void)stream;
	for (index = 0; index < frames; index++)
	{
		int slot = (write + index) & (WEB_AUDIO_RING_FRAMES - 1);

		shared_state.audio_ring[slot * 2] = samples[index * 2];
		shared_state.audio_ring[slot * 2 + 1] = samples[index * 2 + 1];
	}
	__atomic_store_n(&shared_state.audio_write, write + frames, __ATOMIC_SEQ_CST);
	return true;
}

bool SDL_ResumeAudioStreamDevice(SDL_AudioStream *stream)
{
	(void)stream;
	audio_stream.resumed = 1;
	return true;
}
