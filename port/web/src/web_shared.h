/*
WEB_SHARED.H

The state the page (port/web/site/app.js, on the browser's main thread)
shares with the game (a pthread, in a Web Worker) through the WebAssembly
memory. The page finds it with web_shared_state() and reads and writes it
through typed arrays at the offsets below: every member is a 32-bit word,
so the layout is the same on both sides.

- events: keyboard, mouse and focus events, a ring the page writes and
  SDL_PollEvent reads (web_sdl.c);
- gamepads: the state of up to WEB_GAMEPAD_COUNT controllers (the page
  polls the Gamepad API each animation frame; the touch controls are one
  more controller);
- display: the size of the page's canvas in device pixels, and a counter
  the page advances each animation frame, which paces the game's frames;
- audio: a ring of interleaved stereo float samples the game's mixer
  fills and the page's AudioWorklet plays.
*/

#ifndef __HALO_WEB_SHARED_H
#define __HALO_WEB_SHARED_H

#include <stdint.h>

#define WEB_SHARED_MAGIC 0x42455748u /* 'HWEB' */
#define WEB_SHARED_VERSION 1

#define WEB_EVENT_CAPACITY 256
#define WEB_GAMEPAD_COUNT 5
#define WEB_AUDIO_RING_FRAMES 8192 /* a power of two */
/* the rings of packets to and from the other players (web_net.c, net.js) */
#define WEB_NET_OUT_BYTES (1024 * 1024) /* powers of two */
#define WEB_NET_IN_BYTES (2 * 1024 * 1024)
#define WEB_GATEWAY_PEERS 32
#define WEB_PING_PEERS 128
#define WEB_PING_STALE_MS 10000

enum
{
	WEB_EVENT_NONE,
	WEB_EVENT_KEY,          /* a: scancode, b: down, c: repeat, d: keycode */
	WEB_EVENT_MOUSE_MOTION, /* x, y: relative motion in pixels */
	WEB_EVENT_MOUSE_BUTTON, /* a: SDL button, b: down */
	WEB_EVENT_MOUSE_WHEEL,  /* y: notches */
	WEB_EVENT_FOCUS,        /* a: gained */
	WEB_EVENT_GAMEPAD_ADDED,/* a: id */
	WEB_EVENT_QUIT,
};

struct web_event
{
	int32_t type;
	int32_t a, b, c, d;
	float x, y;
	int32_t reserved;
};

/* the SDL_GamepadButton bits and SDL_GamepadAxis values */
struct web_gamepad
{
	int32_t connected;
	int32_t id;          /* the SDL_JoystickID, never 0 */
	int32_t type;        /* SDL_GamepadType */
	uint32_t buttons;    /* 1 << SDL_GamepadButton */
	int32_t axes[6];     /* SDL_GamepadAxis order, -32768..32767 (triggers 0..32767) */
	int32_t rumble_low, rumble_high; /* written by the game, 0..65535 */
	int32_t rumble_serial;           /* advanced with each rumble request */
	int32_t rumble_milliseconds;
};

struct web_shared_state
{
	uint32_t magic;
	uint32_t version;
	uint32_t size;                  /* sizeof(struct web_shared_state) */

	/* events: the page writes at event_write, the game reads at
	event_read; both count without wrapping */
	volatile int32_t event_write;
	volatile int32_t event_read;
	struct web_event events[WEB_EVENT_CAPACITY];

	struct web_gamepad gamepads[WEB_GAMEPAD_COUNT];

	/* the canvas in device pixels, and the animation frame counter */
	volatile int32_t display_width;
	volatile int32_t display_height;
	volatile int32_t frame_counter;
	/* frames the game presented, and whether it waits for each animation
	frame (display.vsync) */
	volatile int32_t frames_presented;
	volatile int32_t vsync;

	/* audio: sample rate, and the ring (WEB_AUDIO_RING_FRAMES stereo
	frames); the game writes at audio_write, the worklet reads at
	audio_read, both counting frames without wrapping */
	volatile int32_t audio_rate;
	volatile int32_t audio_open;
	volatile int32_t audio_write;
	volatile int32_t audio_read;
	volatile int32_t audio_underruns;
	float audio_ring[WEB_AUDIO_RING_FRAMES * 2];

	/* the page's state for the game: nonzero while it is hidden */
	volatile int32_t page_hidden;
	/* the game's state for the page: 1 once the game's window opened */
	volatile int32_t game_started;
	volatile int32_t reserved[16];

	/* online play (web_net.c, port/web/site/net.js): this machine's address
	on the players' network (network byte order, from the page), and the
	packets the game sends to other machines and receives from them; each
	ring counts bytes without wrapping */
	volatile int32_t net_local_address;
	volatile int32_t net_out_write;
	volatile int32_t net_out_read;
	volatile int32_t net_in_write;
	volatile int32_t net_in_read;
	unsigned char net_out[WEB_NET_OUT_BYTES];
	unsigned char net_in[WEB_NET_IN_BYTES];
	/* Native invite relay identity. Peer address is published last; zero
	means an unused slot. Identifiers are two little-endian words (6 bytes). */
	volatile int32_t gateway_enabled;
	volatile int32_t gateway_identifier[2];
	volatile int32_t gateway_peers[WEB_GATEWAY_PEERS][3];
	/* Host RTT table: page publishes under an odd/even sequence lock. All
	addresses use the shared network byte order; -1 means no fresh sample. */
	volatile int32_t ping_sequence, ping_host, ping_epoch, ping_updated, ping_count;
	volatile int32_t ping_peers[WEB_PING_PEERS][2];
};

/* a packet in the rings: this header, then the payload, padded to 4 bytes;
addresses and ports in network byte order */
enum
{
	WEB_PACKET_DATAGRAM = 1,
	WEB_PACKET_OPEN,    /* a stream connects */
	WEB_PACKET_DATA,    /* a stream's bytes */
	WEB_PACKET_CLOSE,   /* a stream's end closed */
	WEB_PACKET_REFUSE,  /* nothing listens where a stream connected */
};

struct web_packet_header
{
	uint32_t size;      /* the whole packet, padded */
	uint32_t kind;
	uint32_t source_ip;
	uint32_t destination_ip;
	uint16_t source_port;
	uint16_t destination_port;
	uint32_t length;    /* of the payload */
};

struct web_shared_state *web_shared_state(void);

#endif
