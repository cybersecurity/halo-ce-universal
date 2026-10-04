/*
XINPUT_SDL.C

Xbox controllers and the debug keyboard for the Linux build.

Port 0 is always connected: it is the keyboard and mouse, merged with the
first SDL gamepad when one is present. Further SDL gamepads take ports 1-3.

In the game the keyboard and mouse are a control scheme of their own
(port/linux/include/halo_keyboard.h): config.toml's [controls] bind each of
the player's actions to up to two keys, mouse buttons or wheel turns
(Settings > Controls Setup changes them), and the game takes the actions
held (halo_keyboard_actions) with the controller's. They press none of its
buttons, but for the pause menu's and the scoreboard's (Start and Back).

In the menus the keys drive the controller, to move about them:
	arrows           D-pad               W A S D          left stick
	space, enter     A                   escape, backspace B
	delete, E        X                   tab              Y
	F1               back
(keys held as the game and the menus switch count only once let go of), the
on-screen keyboard takes what is typed, and the mouse is free and drives a
pointer
(port/linux/include/halo_ui_pointer.h, source/interface/ui_widget.c).
F11 switches between fullscreen and the window, and F12 releases or
recaptures the mouse, always.

Mouse aim does not go through the right stick: the game's look code asks
halo_linux_mouse_look for the motion since its last call and adds it to the
stick's facing change, so aiming is direct rather than rate based.

The game's debug keyboard exists only for the console. Backquote (which
opens it) always reaches the keystroke queue, everything else only while
the console is open, since the game also polls a few keys directly (escape
returns to the main menu). While the console is open the keyboard does not
drive the controller.
*/

#include "platform.h"
#include "sdl_platform.h"
#include "port_config.h"
#include "halo_keyboard.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PORT_COUNT 4
#define VK_OEM_3_BACKQUOTE 0xc0

/* ---------- game hooks */

/* main/console.c */
extern unsigned char console_is_active(void);
/* port/linux/game/chat.c */
extern unsigned char chat_is_active(void);
extern unsigned char chat_key_passes(int virtual_key);

/* ---------- device tables */

XPP_DEVICE_TYPE XDEVICE_TYPE_GAMEPAD_TABLE;
XPP_DEVICE_TYPE XDEVICE_TYPE_MEMORY_UNIT_TABLE;
XPP_DEVICE_TYPE XDEVICE_TYPE_DEBUG_KEYBOARD_TABLE;

struct controller
{
	BOOL open;
	DWORD packet_number;
	XINPUT_GAMEPAD previous;
};

static struct controller controllers[PORT_COUNT];
static struct controller keyboard_device;
static DWORD reported_gamepads = 0;
static BOOL reported_keyboard = FALSE;

/* ---------- mouse */

static pthread_mutex_t mouse_lock = PTHREAD_MUTEX_INITIALIZER;
static float mouse_pending_x, mouse_pending_y;
static unsigned long mouse_polls_unconsumed = 0;
static float mouse_wheel_accumulated = 0.0f;
/* the wheel's switch (wheel_update): when the wheel last moved, until when
Y is held, and whether a scroll is under way */
static Uint64 wheel_moved_ms = 0;
static Uint64 wheel_press_until_ms = 0;
static BOOL wheel_scrolling = FALSE;
/* the way the scroll under way turns: 1 up (away), -1 down */
static int wheel_direction = 0;
/* when port 0's aim last moved, by the mouse and by the right stick
(halo_linux_mouse_aiming) */
static Uint64 mouse_aimed_ms = 0;
static Uint64 stick_aimed_ms = 0;

/* the right stick's deflection that counts as aiming with it, clear of a
worn stick's drift */
#define STICK_AIMING_DEFLECTION 8000

/* (input.mouse_vertical_sensitivity, read with it) */
static float vertical_sensitivity = 1.0f;

static float mouse_sensitivity(void)
{
	static float sensitivity;
	static unsigned long read_at = (unsigned long)-1;

	if (read_at != config_changes())
	{
		read_at = config_changes();
		sensitivity = (float)config_real("input.mouse_sensitivity");
		if (sensitivity <= 0.0f)
			sensitivity = 1.0f;
		vertical_sensitivity = (float)config_real("input.mouse_vertical_sensitivity");
		if (vertical_sensitivity <= 0.0f)
			vertical_sensitivity = sensitivity;
	}
	return sensitivity;
}

/* radians of yaw and pitch for the mouse motion since the last call; the
game adds these to the facing change of the player on gamepad 0 */
int halo_linux_mouse_look(short gamepad_index, float *yaw, float *pitch)
{
	/* radians per pixel of relative motion at sensitivity 1 */
	const float scale = 0.0022f;
	static int invert;
	static unsigned long read_at = (unsigned long)-1;
	float x, y;

	*yaw = 0.0f;
	*pitch = 0.0f;
	if (gamepad_index != 0)
		return FALSE;
	if (read_at != config_changes())
	{
		read_at = config_changes();
		invert = config_boolean("input.invert_mouse");
	}
	pthread_mutex_lock(&mouse_lock);
	x = mouse_pending_x;
	y = mouse_pending_y;
	mouse_pending_x = 0.0f;
	mouse_pending_y = 0.0f;
	mouse_polls_unconsumed = 0;
	pthread_mutex_unlock(&mouse_lock);
	if (x == 0.0f && y == 0.0f)
		return FALSE;
	*yaw = -x * scale * mouse_sensitivity();
	*pitch = (invert ? y : -y) * scale * vertical_sensitivity;
	return TRUE;
}

/* whether the player on the gamepad aims with the mouse (it moved after the
right stick last did) and input.mouse_aim_assist is off: then the view's
magnetism leaves them be (player_control.c); the bullets' autoaim stays */
int halo_linux_mouse_aiming(short gamepad_index)
{
	static int aim_assist;
	static unsigned long read_at = (unsigned long)-1;
	int aiming;

	if (gamepad_index != 0)
		return FALSE;
	if (read_at != config_changes())
	{
		read_at = config_changes();
		aim_assist = config_boolean("input.mouse_aim_assist");
	}
	if (aim_assist)
		return FALSE;
	pthread_mutex_lock(&mouse_lock);
	aiming = mouse_aimed_ms != 0 && mouse_aimed_ms >= stick_aimed_ms;
	pthread_mutex_unlock(&mouse_lock);
	return aiming;
}

/* collects the motion the game has not asked for yet; motion that nobody
consumes for a few polls (menus, cutscenes) is dropped so it cannot jerk
the view later */
static void mouse_poll(const struct platform_input_state *input)
{
	pthread_mutex_lock(&mouse_lock);
	if (++mouse_polls_unconsumed > 4)
	{
		mouse_pending_x = 0.0f;
		mouse_pending_y = 0.0f;
	}
	if (!input->mouse_released)
	{
		mouse_pending_x += input->mouse_dx;
		mouse_pending_y += input->mouse_dy;
		if (input->mouse_dx != 0.0f || input->mouse_dy != 0.0f)
			mouse_aimed_ms = SDL_GetTicks();
		mouse_wheel_accumulated += input->mouse_wheel;
		if (input->mouse_wheel != 0.0f)
			wheel_moved_ms = SDL_GetTicks();
	}
	pthread_mutex_unlock(&mouse_lock);
}

/* ---------- keyboard and mouse as a controller */

static BYTE analog(BOOL down)
{
	return down ? 0xff : 0x00;
}

/* the game's on-screen keyboard is up (platform_text_typing): the keys type
into it (XInputDebugGetKeystroke passes them to the game), but for the
arrows, which move about it, enter (Done, once let go of since it came up)
and escape (cancel) */
static BOOL text_typing;
static BOOL text_typing_enter_armed;
/* (the on-screen keyboard's, and a menu's text field's: menu_functions.c) */
static BOOL text_typing_keyboard, text_typing_field;

static void text_typing_update(void)
{
	BOOL typing = text_typing_keyboard || text_typing_field;

	if (typing && !text_typing)
		text_typing_enter_armed = FALSE;
	text_typing = typing;
}

void platform_text_typing(int typing)
{
	text_typing_keyboard = typing != 0;
	text_typing_update();
}

void platform_text_field(int typing)
{
	text_typing_field = typing != 0;
	text_typing_update();
}

static void typing_gamepad(const struct platform_input_state *input, XINPUT_GAMEPAD *pad)
{
	const unsigned char *k = input->keys;
	BOOL enter = k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_KP_ENTER];

	if (k[SDL_SCANCODE_UP]) pad->wButtons |= XINPUT_GAMEPAD_DPAD_UP;
	if (k[SDL_SCANCODE_DOWN]) pad->wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
	if (k[SDL_SCANCODE_LEFT]) pad->wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
	if (k[SDL_SCANCODE_RIGHT]) pad->wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
	if (!enter)
		text_typing_enter_armed = TRUE;
	else if (text_typing_enter_armed)
		pad->wButtons |= XINPUT_GAMEPAD_START;
	pad->bAnalogButtons[XINPUT_GAMEPAD_B] |= analog(k[SDL_SCANCODE_ESCAPE]);
}

static void keyboard_gamepad(const struct platform_input_state *input, XINPUT_GAMEPAD *pad)
{
	const unsigned char *k = input->keys;
	BOOL mouse = !input->mouse_released;
	const unsigned char *m = input->mouse_buttons;
	int x = 0, y = 0;

	if (text_typing)
	{
		typing_gamepad(input, pad);
		return;
	}

	if (k[SDL_SCANCODE_D]) x++;
	if (k[SDL_SCANCODE_A]) x--;
	if (k[SDL_SCANCODE_W]) y++;
	if (k[SDL_SCANCODE_S]) y--;
	if (x || y)
	{
		/* full deflection, diagonals on the unit circle */
		float length = (x && y) ? 0.70710678f : 1.0f;

		pad->sThumbLX = (SHORT)(x * 32767 * length);
		pad->sThumbLY = (SHORT)(y * 32767 * length);
	}

	if (k[SDL_SCANCODE_UP]) pad->wButtons |= XINPUT_GAMEPAD_DPAD_UP;
	if (k[SDL_SCANCODE_DOWN]) pad->wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
	if (k[SDL_SCANCODE_LEFT]) pad->wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
	if (k[SDL_SCANCODE_RIGHT]) pad->wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
	if (k[SDL_SCANCODE_F1]) pad->wButtons |= XINPUT_GAMEPAD_BACK;

	/* (escape backs out, as backspace does: the pause menu's B resumes the
	game, the main menu's asks to quit; Start would choose, as A does) */
	pad->bAnalogButtons[XINPUT_GAMEPAD_A] |= analog(k[SDL_SCANCODE_SPACE] || k[SDL_SCANCODE_RETURN] ||
		k[SDL_SCANCODE_KP_ENTER]);
	pad->bAnalogButtons[XINPUT_GAMEPAD_B] |= analog(k[SDL_SCANCODE_ESCAPE] || k[SDL_SCANCODE_BACKSPACE] ||
		(mouse && m[SDL_BUTTON_X1]));
#ifdef HALO_ANDROID
	/* the system back key (gesture or button) backs out of menus */
	pad->bAnalogButtons[XINPUT_GAMEPAD_B] |= analog(k[SDL_SCANCODE_AC_BACK]);
#endif
	pad->bAnalogButtons[XINPUT_GAMEPAD_X] |= analog(k[SDL_SCANCODE_DELETE] || k[SDL_SCANCODE_E]);
	pad->bAnalogButtons[XINPUT_GAMEPAD_Y] |= analog(k[SDL_SCANCODE_TAB]);
}

/* the keys held when the game and the menus switch count as up until let go
of: the escape that opens the pause menu does not also back out of it, nor
the one that closes it pause the game again (and the same as chat opens and
closes: the escape or enter that closes it does not play) */
static void keys_held_over_switch(struct platform_input_state *input)
{
	static unsigned char held[SDL_SCANCODE_COUNT];
	static int mode = -1;
	int scancode;
	int current_mode = (input->menus != FALSE) | (chat_is_active() ? 2 : 0);

	if (mode != current_mode)
	{
		mode = current_mode;
		memcpy(held, input->keys, sizeof(held));
	}
	for (scancode = 0; scancode < SDL_SCANCODE_COUNT; scancode++)
	{
		if (!input->keys[scancode])
			held[scancode] = 0;
		else if (held[scancode])
			input->keys[scancode] = 0;
	}
}

/* ---------- the keyboard and mouse's own controls */

#define MAXIMUM_BINDINGS 2

static const char *const binding_settings[NUMBER_OF_HALO_KEYBOARD_ACTIONS] =
{
	"controls.move_forward", "controls.move_backward", "controls.strafe_left", "controls.strafe_right",
	"controls.jump", "controls.crouch", "controls.fire", "controls.throw_grenade", "controls.melee",
	"controls.reload", "controls.zoom", "controls.switch_weapon", "controls.switch_grenade", "controls.action",
	"controls.flashlight", "controls.scoreboard", "controls.pause",
};

static const struct
{
	const char *name;
	int input;
} named_inputs[] =
{
	/* (SDL's names are "," and "Keypad ,", which a list of bindings, split
	at commas, cannot hold) */
	{ "Comma", SDL_SCANCODE_COMMA },
	{ "Keypad Comma", SDL_SCANCODE_KP_COMMA },
	{ "Mouse Left", INPUT_MOUSE + SDL_BUTTON_LEFT },
	{ "Mouse Right", INPUT_MOUSE + SDL_BUTTON_RIGHT },
	{ "Mouse Middle", INPUT_MOUSE + SDL_BUTTON_MIDDLE },
	{ "Mouse 4", INPUT_MOUSE + SDL_BUTTON_X1 },
	{ "Mouse 5", INPUT_MOUSE + SDL_BUTTON_X2 },
	{ "Wheel", INPUT_WHEEL },
	{ "Wheel Up", INPUT_WHEEL_UP },
	{ "Wheel Down", INPUT_WHEEL_DOWN },
};

/* the bindings, read again when config.toml changes; -1 for none */
static int bindings[NUMBER_OF_HALO_KEYBOARD_ACTIONS][MAXIMUM_BINDINGS];
static unsigned long bindings_read_at = (unsigned long)-1;
static unsigned long keyboard_actions_held;

/* (the C library's case: SDL's string functions are not the Android
guest's) */
static int same_name(const char *a, const char *b)
{
	for (; *a && *b; a++, b++)
	{
		if ((*a >= 'a' && *a <= 'z' ? *a - 32 : *a) != (*b >= 'a' && *b <= 'z' ? *b - 32 : *b))
			return FALSE;
	}
	return *a == *b;
}

int halo_input_from_name(const char *name)
{
	SDL_Scancode scancode;
	size_t index;

	for (index = 0; index < sizeof(named_inputs) / sizeof(named_inputs[0]); index++)
	{
		if (same_name(name, named_inputs[index].name))
			return named_inputs[index].input;
	}
	/* (the other mouse buttons, as halo_input_name names them) */
	if (!strncmp(name, "Mouse ", 6) && name[6] >= '1' && name[6] <= '9' && !name[7] &&
		name[6] - '0' < PLATFORM_MOUSE_BUTTON_COUNT)
	{
		return INPUT_MOUSE + (name[6] - '0');
	}
	scancode = SDL_GetScancodeFromName(name);
	return scancode != SDL_SCANCODE_UNKNOWN ? (int)scancode : -1;
}

void halo_input_name(int input, char *name, size_t size)
{
	size_t index;

	for (index = 0; index < sizeof(named_inputs) / sizeof(named_inputs[0]); index++)
	{
		if (named_inputs[index].input == input)
		{
			snprintf(name, size, "%s", named_inputs[index].name);
			return;
		}
	}
	if (input >= 0 && input < SDL_SCANCODE_COUNT && *SDL_GetScancodeName((SDL_Scancode)input))
		snprintf(name, size, "%s", SDL_GetScancodeName((SDL_Scancode)input));
	else if (input >= INPUT_MOUSE && input < INPUT_WHEEL)
		snprintf(name, size, "Mouse %d", input - INPUT_MOUSE);
	else
		snprintf(name, size, "%s", "");
}

static void bindings_read(void)
{
	int action;

	if (bindings_read_at == config_changes())
		return;
	bindings_read_at = config_changes();
	for (action = 0; action < NUMBER_OF_HALO_KEYBOARD_ACTIONS; action++)
	{
		const char *text = config_string(binding_settings[action]);
		int slot;

		for (slot = 0; slot < MAXIMUM_BINDINGS; slot++)
		{
			char name[64];
			size_t length;

			bindings[action][slot] = -1;
			while (*text == ' ' || *text == ',')
				text++;
			length = strcspn(text, ",");
			if (!length)
				continue;
			snprintf(name, sizeof(name), "%.*s", (int)length, text);
			while (*name && name[strlen(name) - 1] == ' ')
				name[strlen(name) - 1] = 0;
			bindings[action][slot] = halo_input_from_name(name);
			if (bindings[action][slot] < 0)
				platform_log("controls: %s has no key or button named \"%s\"", binding_settings[action], name);
			text += length;
		}
	}
}

static BOOL input_held(const struct platform_input_state *input, int code)
{
	BOOL wheel = SDL_GetTicks() < wheel_press_until_ms;

	if (code < 0)
		return FALSE;
	if (code < SDL_SCANCODE_COUNT)
		return input->keys[code] != 0;
	if (code < INPUT_WHEEL)
		return !input->mouse_released && input->mouse_buttons[code - INPUT_MOUSE];
	if (code == INPUT_WHEEL)
		return wheel;
	return wheel && wheel_direction == (code == INPUT_WHEEL_UP ? 1 : -1);
}

/* in the game: the actions held, and the controller's Start and Back for
the pause menu and the scoreboard */
static void keyboard_controls(const struct platform_input_state *input, XINPUT_GAMEPAD *pad)
{
	unsigned long held = 0;
	int action, slot;

	bindings_read();
	for (action = 0; action < NUMBER_OF_HALO_KEYBOARD_ACTIONS; action++)
	{
		for (slot = 0; slot < MAXIMUM_BINDINGS; slot++)
		{
			if (input_held(input, bindings[action][slot]))
				held |= 1UL << action;
		}
	}
	if (held & (1UL << HALO_KEYBOARD_PAUSE))
		pad->wButtons |= XINPUT_GAMEPAD_START;
	if (held & (1UL << HALO_KEYBOARD_SCOREBOARD))
		pad->wButtons |= XINPUT_GAMEPAD_BACK;
	keyboard_actions_held = held;
}

unsigned long halo_keyboard_actions(short controller_index)
{
	return controller_index == 0 ? keyboard_actions_held : 0;
}

/* A scroll of the wheel switches weapons once: it holds Y for WHEEL_PRESS_MS
once the wheel has turned a notch, and the scroll lasts until the wheel has
been still for WHEEL_SCROLL_GAP_MS. One notch often arrives as several events
over a few tens of milliseconds (high-resolution and smooth-scrolling
wheels), and one flick turns several notches; switching for each would bring
the same weapon straight back. Timed in milliseconds, not polls: polls come
once a frame, at the display's refresh rate. */
#define WHEEL_PRESS_MS 50
#define WHEEL_SCROLL_GAP_MS 200

/* debug.test_input "bot:<seed>": a scripted player for the automated
network tests (port/linux/game/network_test.c), different for each seed:
it walks and strafes in circles, turns, fires every few seconds, jumps now
and then and throws a grenade every seven seconds; "look:<seed>" stands
still, only turning and looking up and down (where remote players aim and
whether they stand) */
static int test_input_holding_action;
static Uint64 test_input_holding_action_since;

/* the automated tests (port/linux/game/network_test.c): the scripted player
stands still, holding the action button (X: picking up, swapping weapons)
after a second */
void test_input_hold_action(int hold)
{
	if (hold && !test_input_holding_action)
		test_input_holding_action_since = SDL_GetTicks();
	test_input_holding_action = hold;
}

static void test_input_gamepad(XINPUT_GAMEPAD *pad)
{
	static int checked;
	static int seed = -1;
	static int looking;
	double t;

	if (!checked)
	{
		const char *setting = config_string("debug.test_input");

		checked = 1;
		if (!strncmp(setting, "bot:", 4))
			seed = atoi(setting + 4);
		else if (!strcmp(setting, "bot"))
			seed = 0;
		else if (!strncmp(setting, "look:", 5))
		{
			seed = atoi(setting + 5);
			looking = 1;
		}
	}
	if (seed < 0)
		return;
	if (test_input_holding_action)
	{
		/* (standing still, the button held from a second on) */
		if (SDL_GetTicks() - test_input_holding_action_since >= 1000)
			pad->bAnalogButtons[XINPUT_GAMEPAD_X] = 255;
		return;
	}
	t = (double)SDL_GetTicks() / 1000.0 + seed * 1.7;
	if (looking)
	{
		pad->sThumbRX = (SHORT)(sin(t * 0.5) * 14000.0);
		pad->sThumbRY = (SHORT)(sin(t * 0.3) * 32000.0);
		return;
	}
	pad->sThumbLY = (SHORT)(sin(t * 0.9) * 32000.0);
	pad->sThumbLX = (SHORT)(cos(t * 0.6 + seed) * 20000.0);
	pad->sThumbRX = (SHORT)(sin(t * 0.4) * 14000.0);
	if (fmod(t, 3.0) < 0.3)
		pad->bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER] = 255;
	if (fmod(t, 5.0) < 0.1)
		pad->bAnalogButtons[XINPUT_GAMEPAD_A] = 255;
	if (fmod(t, 7.0) < 0.2)
		pad->bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER] = 255;
}

static void wheel_update(void)
{
	Uint64 now = SDL_GetTicks();

	pthread_mutex_lock(&mouse_lock);
	if (!wheel_scrolling)
	{
		if (fabsf(mouse_wheel_accumulated) >= 1.0f)
		{
			wheel_scrolling = TRUE;
			wheel_direction = mouse_wheel_accumulated > 0.0f ? 1 : -1;
			wheel_press_until_ms = now + WHEEL_PRESS_MS;
		}
	}
	else if (now >= wheel_press_until_ms && now - wheel_moved_ms >= WHEEL_SCROLL_GAP_MS)
	{
		wheel_scrolling = FALSE;
		mouse_wheel_accumulated = 0.0f;
	}
	pthread_mutex_unlock(&mouse_lock);
}

/* ---------- SDL gamepads */

/* the SDL gamepads in connection order, at most one per port */
static int sdl_gamepads(SDL_Gamepad *gamepads[PORT_COUNT])
{
	SDL_JoystickID *ids;
	int count = 0, index, found = 0;

	memset(gamepads, 0, sizeof(SDL_Gamepad *) * PORT_COUNT);
	ids = SDL_GetGamepads(&count);
	if (!ids)
		return 0;
#ifdef HALO_ANDROID
	{
		/* Android can list input devices with a few gamepad buttons (the
		emulator's keyboard, some phones' key devices) as generic gamepads:
		recognised controllers take the first ports */
		int pass;

		for (pass = 0; pass < 2; pass++)
		{
			for (index = 0; index < count && found < PORT_COUNT; index++)
			{
				SDL_Gamepad *gamepad = SDL_GetGamepadFromID(ids[index]);
				SDL_GamepadType type;
				BOOL recognised;

				if (!gamepad)
					continue;
				type = SDL_GetGamepadType(gamepad);
				recognised = type != SDL_GAMEPAD_TYPE_UNKNOWN && type != SDL_GAMEPAD_TYPE_STANDARD;
				if (recognised == (pass == 0))
					gamepads[found++] = gamepad;
			}
		}
	}
#else
	for (index = 0; index < count && found < PORT_COUNT; index++)
	{
		SDL_Gamepad *gamepad = SDL_GetGamepadFromID(ids[index]);

		if (gamepad)
			gamepads[found++] = gamepad;
	}
#endif
	SDL_free(ids);
	return found;
}

static SHORT stick(Sint16 value, BOOL flip)
{
	int result = flip ? -(int)value - 1 : value;

	if (result < -32768) result = -32768;
	if (result > 32767) result = 32767;
	return (SHORT)result;
}

static void merge_button(XINPUT_GAMEPAD *pad, int analog_index, BOOL down)
{
	if (down)
		pad->bAnalogButtons[analog_index] = 0xff;
}

static void sdl_gamepad_state(SDL_Gamepad *gamepad, XINPUT_GAMEPAD *pad)
{
	static const struct
	{
		SDL_GamepadButton button;
		WORD mask;
	} digital[] =
	{
		{ SDL_GAMEPAD_BUTTON_DPAD_UP, XINPUT_GAMEPAD_DPAD_UP },
		{ SDL_GAMEPAD_BUTTON_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_DOWN },
		{ SDL_GAMEPAD_BUTTON_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_LEFT },
		{ SDL_GAMEPAD_BUTTON_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT },
		{ SDL_GAMEPAD_BUTTON_START, XINPUT_GAMEPAD_START },
		{ SDL_GAMEPAD_BUTTON_BACK, XINPUT_GAMEPAD_BACK },
		{ SDL_GAMEPAD_BUTTON_LEFT_STICK, XINPUT_GAMEPAD_LEFT_THUMB },
		{ SDL_GAMEPAD_BUTTON_RIGHT_STICK, XINPUT_GAMEPAD_RIGHT_THUMB },
	};
	int index;
	int left_trigger, right_trigger;
	SHORT value;

	for (index = 0; index < (int)(sizeof(digital) / sizeof(digital[0])); index++)
	{
		if (SDL_GetGamepadButton(gamepad, digital[index].button))
			pad->wButtons |= digital[index].mask;
	}
	merge_button(pad, XINPUT_GAMEPAD_A, SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_SOUTH));
	merge_button(pad, XINPUT_GAMEPAD_B, SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_EAST));
	merge_button(pad, XINPUT_GAMEPAD_X, SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_WEST));
	merge_button(pad, XINPUT_GAMEPAD_Y, SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_NORTH));
	/* the Duke's white and black buttons sit where later pads have shoulders */
	merge_button(pad, XINPUT_GAMEPAD_WHITE, SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER));
	merge_button(pad, XINPUT_GAMEPAD_BLACK, SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER));

	left_trigger = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) * 255 / 32767;
	right_trigger = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) * 255 / 32767;
	if (left_trigger > pad->bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER])
		pad->bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER] = (BYTE)left_trigger;
	if (right_trigger > pad->bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER])
		pad->bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER] = (BYTE)right_trigger;

	/* a stick only overrides the keyboard when it is pushed further */
	value = stick(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX), FALSE);
	if (abs(value) > abs(pad->sThumbLX)) pad->sThumbLX = value;
	value = stick(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY), TRUE);
	if (abs(value) > abs(pad->sThumbLY)) pad->sThumbLY = value;
	value = stick(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTX), FALSE);
	if (abs(value) > abs(pad->sThumbRX)) pad->sThumbRX = value;
	value = stick(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTY), TRUE);
	if (abs(value) > abs(pad->sThumbRY)) pad->sThumbRY = value;
}

/* ---------- XAPI */

VOID WINAPI XInitDevices(DWORD preallocation_type_count, PXDEVICE_PREALLOC_TYPE preallocation_types)
{
	(void)preallocation_type_count;
	(void)preallocation_types;
	platform_sdl_initialize();
}

static DWORD connected_gamepads(void)
{
	SDL_Gamepad *gamepads[PORT_COUNT];
	int count = sdl_gamepads(gamepads);
	DWORD mask = XDEVICE_PORT0_MASK;
	int port;

	/* the first pad shares port 0 with the keyboard */
	for (port = 1; port < count; port++)
		mask |= 1UL << port;
	return mask;
}

BOOL WINAPI XGetDeviceChanges(PXPP_DEVICE_TYPE device_type, PDWORD insertions, PDWORD removals)
{
	*insertions = 0;
	*removals = 0;
	if (device_type == XDEVICE_TYPE_GAMEPAD)
	{
		DWORD connected = connected_gamepads();

		*insertions = connected & ~reported_gamepads;
		*removals = reported_gamepads & ~connected;
		reported_gamepads = connected;
	}
	else if (device_type == XDEVICE_TYPE_DEBUG_KEYBOARD)
	{
		if (!reported_keyboard)
		{
			*insertions = 1;
			reported_keyboard = TRUE;
		}
	}
	return *insertions || *removals;
}

HANDLE WINAPI XInputOpen(PXPP_DEVICE_TYPE device_type, DWORD port, DWORD slot,
	PXINPUT_POLLING_PARAMETERS polling_parameters)
{
	(void)slot;
	(void)polling_parameters;
	if (device_type == XDEVICE_TYPE_GAMEPAD && port < PORT_COUNT)
	{
		memset(&controllers[port], 0, sizeof(controllers[port]));
		controllers[port].open = TRUE;
		return (HANDLE)&controllers[port];
	}
	if (device_type == XDEVICE_TYPE_DEBUG_KEYBOARD && port == 0)
	{
		keyboard_device.open = TRUE;
		return (HANDLE)&keyboard_device;
	}
	SetLastError(ERROR_DEVICE_NOT_CONNECTED);
	return NULL;
}

VOID WINAPI XInputClose(HANDLE device)
{
	struct controller *controller = (struct controller *)device;

	if (controller)
		controller->open = FALSE;
}

static int controller_port(HANDLE device)
{
	int port;

	for (port = 0; port < PORT_COUNT; port++)
	{
		if (device == (HANDLE)&controllers[port] && controllers[port].open)
			return port;
	}
	return -1;
}

DWORD WINAPI XInputGetState(HANDLE device, PXINPUT_STATE state)
{
	int port = controller_port(device);
	SDL_Gamepad *gamepads[PORT_COUNT];
	int count;

	memset(state, 0, sizeof(*state));
	if (port < 0)
		return ERROR_DEVICE_NOT_CONNECTED;
	platform_pump_events();
	count = sdl_gamepads(gamepads);
	if (port == 0)
	{
		struct platform_input_state input;

		platform_input_read(&input, TRUE);
		mouse_poll(&input);
		wheel_update();
		keyboard_actions_held = 0;
		keys_held_over_switch(&input);
		if (!console_is_active() && !chat_is_active())
		{
			if (input.menus)
				keyboard_gamepad(&input, &state->Gamepad);
			else
				keyboard_controls(&input, &state->Gamepad);
		}
		if (count > 0)
			sdl_gamepad_state(gamepads[0], &state->Gamepad);
		test_input_gamepad(&state->Gamepad);
		if (abs(state->Gamepad.sThumbRX) > STICK_AIMING_DEFLECTION ||
			abs(state->Gamepad.sThumbRY) > STICK_AIMING_DEFLECTION)
		{
			pthread_mutex_lock(&mouse_lock);
			stick_aimed_ms = SDL_GetTicks();
			pthread_mutex_unlock(&mouse_lock);
		}
	}
	else if (port < count)
	{
		sdl_gamepad_state(gamepads[port], &state->Gamepad);
	}

	if (memcmp(&state->Gamepad, &controllers[port].previous, sizeof(state->Gamepad)))
	{
		controllers[port].packet_number++;
		controllers[port].previous = state->Gamepad;
	}
	state->dwPacketNumber = controllers[port].packet_number;
	return ERROR_SUCCESS;
}

DWORD WINAPI XInputSetState(HANDLE device, PXINPUT_FEEDBACK feedback)
{
	int port = controller_port(device);
	SDL_Gamepad *gamepads[PORT_COUNT];
	int count;

	if (!feedback)
		return ERROR_INVALID_PARAMETER;
	feedback->Header.dwStatus = ERROR_SUCCESS;
	if (port < 0)
		return ERROR_DEVICE_NOT_CONNECTED;
	count = sdl_gamepads(gamepads);
	if (port < count)
	{
		/* the game refreshes the motors every frame; rumble a little longer
		than that so they do not stutter */
		SDL_RumbleGamepad(gamepads[port], feedback->Rumble.wLeftMotorSpeed,
			feedback->Rumble.wRightMotorSpeed, 100);
	}
	return ERROR_SUCCESS;
}

DWORD WINAPI XInputDebugInitKeyboardQueue(PXINPUT_DEBUG_KEYQUEUE_PARAMETERS parameters)
{
	(void)parameters;
	return ERROR_SUCCESS;
}

DWORD WINAPI XInputDebugGetKeystroke(PXINPUT_DEBUG_KEYSTROKE keystroke)
{
	struct platform_keystroke next;

	memset(keystroke, 0, sizeof(*keystroke));
	while (platform_next_keystroke(&next))
	{
		BOOL key_up = (next.flags & XINPUT_DEBUG_KEYSTROKE_FLAG_KEYUP) != 0;

		/* key ups always pass, so no key is left latched down; while typing,
		escape does not (it cancels, as B: the game's own escape leaves the
		menus, main.c); chat's keys, and every key while it is open */
		if (key_up || next.virtual_key == VK_OEM_3_BACKQUOTE || console_is_active() ||
			chat_key_passes(next.virtual_key) ||
			(text_typing && next.virtual_key != 0x1B /* escape */))
		{
			keystroke->VirtualKey = next.virtual_key;
			keystroke->Ascii = next.ascii;
			keystroke->Flags = next.flags;
			return ERROR_SUCCESS;
		}
	}
	return ERROR_HANDLE_EOF;
}
