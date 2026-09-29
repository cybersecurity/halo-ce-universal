/*
XINPUT_SDL.C

Xbox controllers and the debug keyboard for the Linux build.

Port 0 is always connected: it is the keyboard and mouse, merged with the
first SDL gamepad when one is present. Further SDL gamepads take ports 1-3.

Keyboard and mouse (port 0):
	W A S D          left stick          arrows           D-pad
	mouse            aim (see halo_linux_mouse_look)
	left button      right trigger       right button, G  left trigger
	space, enter     A                   F, backspace, X1 B
	E, R             X                   tab, wheel       Y
	Q                white               X                black
	left ctrl, C     left stick click    Z, middle button right stick click
	escape           start               F1               back
	F12              release or recapture the mouse

In the menus the mouse is free and drives a pointer instead
(port/linux/include/halo_ui_pointer.h, source/interface/ui_widget.c): its
motion, buttons and wheel do not reach the controller then.

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
#include "input_bindings.h"

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

static float mouse_sensitivity(void)
{
	static float sensitivity = -1.0f;

	if (sensitivity < 0.0f)
	{
		sensitivity = (float)config_real("input.mouse_sensitivity");
		if (!isfinite(sensitivity) || sensitivity <= 0.0f)
			sensitivity = 1.0f;
	}
	return sensitivity;
}

/* radians of yaw and pitch for the mouse motion since the last call; the
game adds these to the facing change of the player on gamepad 0 */
int halo_linux_mouse_look(short gamepad_index, float *yaw, float *pitch)
{
	/* radians per pixel of relative motion at sensitivity 1 */
	const float scale = 0.0022f;
	static int invert = -1;
	float x, y;

	*yaw = 0.0f;
	*pitch = 0.0f;
	if (gamepad_index != 0)
		return FALSE;
	if (invert < 0)
		invert = config_boolean("input.invert_mouse");
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
	*pitch = (invert ? y : -y) * scale * mouse_sensitivity();
	return TRUE;
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
	if (input->focused && !input->mouse_released)
	{
		mouse_pending_x += input->mouse_dx;
		mouse_pending_y += input->mouse_dy;
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

enum { B_FORWARD, B_BACKWARD, B_LEFT, B_RIGHT, B_JUMP, B_MELEE, B_ACTION,
 B_WEAPON, B_FLASHLIGHT, B_GRENADE_TYPE, B_GRENADE, B_FIRE, B_CROUCH, B_ZOOM, B_COUNT };
static struct input_binding bindings[B_COUNT];
static void load_bindings(void)
{
 static int loaded;
 static const char *names[B_COUNT] = {"forward","backward","left","right","jump","melee","action",
  "change_weapon","flashlight","change_grenade","grenade","fire","crouch","zoom"};
 static const char *defaults[B_COUNT] = {"W","S","A","D","Space","F,Mouse4","E,R",
  "Tab,Wheel","Q","X",
#ifdef HALO_MACOS
  "G","Mouse1","LeftCtrl,C","Z,Mouse2,Mouse3"
#else
  "G,Mouse2","Mouse1","LeftCtrl,C","Z,Mouse3"
#endif
 };
 int i;
 if(loaded) return;
 for(i=0;i<B_COUNT;i++) {
  char name[64]; const char *value;
  snprintf(name,sizeof(name),"input.%s",names[i]); value=config_string(name);
  if(!input_binding_parse(value,&bindings[i])) {
   platform_log("invalid binding %s; using %s",name,defaults[i]);
   input_binding_parse(defaults[i],&bindings[i]);
  }
 }
 loaded=1;
}
static int physical_gamepad_present;
static SDL_GamepadType physical_gamepad_type;
#ifdef HALO_MACOS
/* HUD enum values are original Xbox icon IDs. Labels follow the configured
 * keyboard binding and leave controller artwork intact for a physical pad. */
const wchar_t *halo_macos_control_prompt(short icon)
{
 static wchar_t labels[18][40];
 static const char *names[18] = {"jump","melee","action","change_weapon","change_grenade","flashlight",
  "grenade","fire",NULL,NULL,NULL,NULL,NULL,NULL,"crouch","zoom",NULL,NULL};
 static const char *fixed[18] = {NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,"Up","Down","Left","Right","Esc","F1",NULL,NULL,"WASD","Mouse"};
 const char *text; char name[64], movement[40]; unsigned int i;
 static const char *fallback[18] = {"Space","F","E","Tab","X","Q","G","Mouse1",NULL,NULL,NULL,NULL,NULL,NULL,"LeftCtrl","Z",NULL,NULL};
 struct input_binding check;
 if(icon<0 || icon>=18) return NULL;
 if(physical_gamepad_present) {
  static const wchar_t *sony[18]={L"Cross",L"Circle",L"Square",L"Triangle",L"R1",L"L1",L"L2",L"R2",
   L"D-pad Up",L"D-pad Down",L"D-pad Left",L"D-pad Right",L"Options",L"Share",L"L3",L"R3",L"Left Stick",L"Right Stick"};
  if(physical_gamepad_type==SDL_GAMEPAD_TYPE_PS3 || physical_gamepad_type==SDL_GAMEPAD_TYPE_PS4 || physical_gamepad_type==SDL_GAMEPAD_TYPE_PS5) return sony[icon];
  return NULL;
 }
 if(names[icon]) { snprintf(name,sizeof(name),"input.%s",names[icon]); text=config_string(name); }
 else text=fixed[icon];
 if(names[icon] && !input_binding_parse(text,&check)) text=fallback[icon];
 if(icon==16) {
  const char *f=config_string("input.forward"), *b=config_string("input.backward");
  const char *l=config_string("input.left"), *r=config_string("input.right");
  if(strcmp(f,"W") || strcmp(b,"S") || strcmp(l,"A") || strcmp(r,"D")) {
   snprintf(movement,sizeof(movement),"%s/%s/%s/%s",f,b,l,r); text=movement;
  }
 }
 if(!text || !*text) text="Unbound";
 for(i=0;i<39 && text[i] && text[i]!=',';i++) labels[icon][i]=(wchar_t)(unsigned char)text[i];
 labels[icon][i]=0;
 return labels[icon];
}
#endif
void halo_input_focus_lost(void)
{
 pthread_mutex_lock(&mouse_lock);
 mouse_pending_x=mouse_pending_y=mouse_wheel_accumulated=0.0f;
 mouse_polls_unconsumed=0; wheel_moved_ms=wheel_press_until_ms=0; wheel_scrolling=FALSE;
 pthread_mutex_unlock(&mouse_lock);
}

static void keyboard_gamepad(const struct platform_input_state *input, XINPUT_GAMEPAD *pad)
{
	const unsigned char *k = input->keys;
	BOOL mouse = !input->mouse_released;
	const unsigned char *m = input->mouse_buttons;
	int x = 0, y = 0;
	load_bindings();
#define DOWN(b) input_binding_down(&bindings[b], k, m, mouse)

	if (DOWN(B_RIGHT)) x++;
	if (DOWN(B_LEFT)) x--;
	if (DOWN(B_FORWARD)) y++;
	if (DOWN(B_BACKWARD)) y--;
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
	if (k[SDL_SCANCODE_ESCAPE]) pad->wButtons |= XINPUT_GAMEPAD_START;
	if (k[SDL_SCANCODE_F1]) pad->wButtons |= XINPUT_GAMEPAD_BACK;
	if (DOWN(B_CROUCH)) pad->wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
	if (DOWN(B_ZOOM)) pad->wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;

	pad->bAnalogButtons[XINPUT_GAMEPAD_A] |= analog(DOWN(B_JUMP) || k[SDL_SCANCODE_RETURN] ||
		k[SDL_SCANCODE_KP_ENTER]);
	pad->bAnalogButtons[XINPUT_GAMEPAD_B] |= analog(DOWN(B_MELEE) || k[SDL_SCANCODE_BACKSPACE]);
#ifdef HALO_ANDROID
	/* the system back key (gesture or button) backs out of menus */
	pad->bAnalogButtons[XINPUT_GAMEPAD_B] |= analog(k[SDL_SCANCODE_AC_BACK]);
#endif
	pad->bAnalogButtons[XINPUT_GAMEPAD_X] |= analog(DOWN(B_ACTION));
	pad->bAnalogButtons[XINPUT_GAMEPAD_Y] |= analog(DOWN(B_WEAPON) || (bindings[B_WEAPON].wheel && SDL_GetTicks() < wheel_press_until_ms));
	pad->bAnalogButtons[XINPUT_GAMEPAD_WHITE] |= analog(DOWN(B_FLASHLIGHT));
	pad->bAnalogButtons[XINPUT_GAMEPAD_BLACK] |= analog(DOWN(B_GRENADE_TYPE));
	pad->bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER] |= analog(DOWN(B_GRENADE));
	pad->bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER] |= analog(DOWN(B_FIRE));
#undef DOWN
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
and then and throws a grenade every seven seconds */
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
	static int movement_only;
	double t;

	if (!checked)
	{
		const char *setting = config_string("debug.test_input");

		checked = 1;
		if (!strncmp(setting, "move:", 5))
		{
			seed = atoi(setting + 5);
			movement_only = 1;
		}
		else if (!strncmp(setting, "bot:", 4))
			seed = atoi(setting + 4);
		else if (!strcmp(setting, "bot"))
			seed = 0;
	}
	if (seed < 0)
		return;
	if (movement_only)
		memset(pad, 0, sizeof(*pad));
	if (test_input_holding_action && !movement_only)
	{
		/* (standing still, the button held from a second on) */
		if (SDL_GetTicks() - test_input_holding_action_since >= 1000)
			pad->bAnalogButtons[XINPUT_GAMEPAD_X] = 255;
		return;
	}
	t = (double)SDL_GetTicks() / 1000.0 + seed * 1.7;
	pad->sThumbLY = (SHORT)(sin(t * 0.9) * 32000.0);
	pad->sThumbLX = (SHORT)(cos(t * 0.6 + seed) * 20000.0);
	pad->sThumbRX = (SHORT)(sin(t * 0.4) * 14000.0);
	if (movement_only)
		return;
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
#ifdef HALO_MACOS
/* Four fixed player slots. Enumerate/allocate an SDL list only initially or
 * after hot-plug. A token lookup also recovers a missed removal notification. */
static pthread_mutex_t gamepad_cache_lock = PTHREAD_MUTEX_INITIALIZER;
static SDL_JoystickID assigned_gamepads[PORT_COUNT];
static SDL_Gamepad *cached_gamepads[PORT_COUNT];
static int cached_gamepad_count;
static BOOL gamepad_cache_dirty = TRUE;
void halo_input_gamepads_changed(void)
{
 pthread_mutex_lock(&gamepad_cache_lock);
 gamepad_cache_dirty = TRUE;
 pthread_mutex_unlock(&gamepad_cache_lock);
}
#endif

static int sdl_gamepads(SDL_Gamepad *gamepads[PORT_COUNT])
{
	SDL_JoystickID *ids;
	int count = 0, index, found = 0;

#ifdef HALO_MACOS
 pthread_mutex_lock(&gamepad_cache_lock);
 for(index=0;index<PORT_COUNT;index++)
  if(assigned_gamepads[index] && SDL_GetGamepadFromID(assigned_gamepads[index]) != cached_gamepads[index])
   gamepad_cache_dirty=TRUE;
 if(!gamepad_cache_dirty) {
  memcpy(gamepads,cached_gamepads,sizeof(cached_gamepads));
  found=cached_gamepad_count;
  pthread_mutex_unlock(&gamepad_cache_lock);
  return found;
 }
#endif
	memset(gamepads, 0, sizeof(SDL_Gamepad *) * PORT_COUNT);
	ids = SDL_GetGamepads(&count);
	if (!ids) {
#ifdef HALO_MACOS
  pthread_mutex_unlock(&gamepad_cache_lock);
#endif
  return 0;
 }
#ifdef HALO_MACOS
 {
  int port;
  /* Preserve occupied slots when another controller disconnects. */
  for(port=0;port<PORT_COUNT;port++) {
   BOOL present=FALSE;
   for(index=0;index<count;index++) if(ids[index]==assigned_gamepads[port]) present=TRUE;
   if(!present) assigned_gamepads[port]=0;
  }
  for(index=0;index<count;index++) {
   BOOL present=FALSE;
   for(port=0;port<PORT_COUNT;port++) if(assigned_gamepads[port]==ids[index]) present=TRUE;
   if(!present) for(port=0;port<PORT_COUNT;port++) if(!assigned_gamepads[port]) { assigned_gamepads[port]=ids[index];break; }
  }
  for(port=0;port<PORT_COUNT;port++) if(assigned_gamepads[port]) {
   gamepads[port]=SDL_GetGamepadFromID(assigned_gamepads[port]);
   if(!gamepads[port]) gamepads[port]=SDL_OpenGamepad(assigned_gamepads[port]);
   if(gamepads[port]) found=port+1;
  }
 }
#elif defined(HALO_ANDROID)
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
#ifdef HALO_MACOS
 memcpy(cached_gamepads,gamepads,sizeof(cached_gamepads));
 cached_gamepad_count=found;gamepad_cache_dirty=FALSE;
 pthread_mutex_unlock(&gamepad_cache_lock);
#endif
	return found;
}

static SHORT stick(Sint16 value, BOOL flip)
{
	int result = flip ? -(int)value : value;

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
	left_trigger = left_trigger < 0 ? 0 : left_trigger > 255 ? 255 : left_trigger;
	right_trigger = right_trigger < 0 ? 0 : right_trigger > 255 ? 255 : right_trigger;
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
		if (gamepads[port]) mask |= 1UL << port;
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

		physical_gamepad_present = gamepads[0] != NULL;
		physical_gamepad_type = physical_gamepad_present ? SDL_GetGamepadType(gamepads[0]) : SDL_GAMEPAD_TYPE_UNKNOWN;
		platform_input_read(&input, TRUE);
		mouse_poll(&input);
		wheel_update();
		if (input.focused && !console_is_active())
			keyboard_gamepad(&input, &state->Gamepad);
		if (gamepads[0])
			sdl_gamepad_state(gamepads[0], &state->Gamepad);
		test_input_gamepad(&state->Gamepad);
	}
	else if (port < count && gamepads[port])
	{
		sdl_gamepad_state(gamepads[port], &state->Gamepad);
	}
 else return ERROR_DEVICE_NOT_CONNECTED;

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
 int port=controller_port(device); SDL_Gamepad *gamepads[PORT_COUNT]; int count;
 DWORD status=ERROR_DEVICE_NOT_CONNECTED;
 if(!feedback) return ERROR_INVALID_PARAMETER;
 if(port>=0) {
  count=sdl_gamepads(gamepads);
  if(port<count && gamepads[port])
   status=SDL_RumbleGamepad(gamepads[port],feedback->Rumble.wLeftMotorSpeed,feedback->Rumble.wRightMotorSpeed,100)?ERROR_SUCCESS:50; /* ERROR_NOT_SUPPORTED */
  else if(port==0) status=50; /* Keyboard/mouse has no motors. */
 }
 feedback->Header.dwStatus=status;
 if(feedback->Header.hEvent) SetEvent(feedback->Header.hEvent);
 return status;
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

		/* key ups always pass, so no key is left latched down */
		if (key_up || next.virtual_key == VK_OEM_3_BACKQUOTE || console_is_active())
		{
			keystroke->VirtualKey = next.virtual_key;
			keystroke->Ascii = next.ascii;
			keystroke->Flags = next.flags;
			return ERROR_SUCCESS;
		}
	}
	return ERROR_HANDLE_EOF;
}
