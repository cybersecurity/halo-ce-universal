/*
TOUCH_BACKEND_VPAD.C

The touch controls as an SDL virtual gamepad. The guest's platform layer
(port/linux/src/xinput_sdl.c) finds it with SDL_GetGamepads like any other
controller, so it needs no change.

The buttons follow the Xbox layout of port/android/README.md, "Controls":
fire = right trigger, grenade = left trigger, jump = A, action/reload = X,
melee = B, change weapon = Y, crouch = left stick click, zoom = right stick
click, flashlight = white (SDL: left shoulder), pause = start, scoreboard =
back. The view is not here: the swipe reaches the game as mouse motion
(TC_TakeLookMotion, host_sdl_poll_event).
*/

#include "host.h"
#include "touch_controls.h"

static SDL_Joystick *joystick;
static SDL_JoystickID joystick_id;
static bool subsystem_started;

static const struct
{
	TC_Action action;
	SDL_GamepadButton button;
} button_map[] =
{
	{ TC_ACT_JUMP, SDL_GAMEPAD_BUTTON_SOUTH },
	{ TC_ACT_MELEE, SDL_GAMEPAD_BUTTON_EAST },
	{ TC_ACT_ACTION_RELOAD, SDL_GAMEPAD_BUTTON_WEST },
	{ TC_ACT_SWITCH_WEAPON, SDL_GAMEPAD_BUTTON_NORTH },
	{ TC_ACT_CROUCH, SDL_GAMEPAD_BUTTON_LEFT_STICK },
	{ TC_ACT_ZOOM, SDL_GAMEPAD_BUTTON_RIGHT_STICK },
	{ TC_ACT_FLASHLIGHT, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER },
	{ TC_ACT_PAUSE, SDL_GAMEPAD_BUTTON_START },
	{ TC_ACT_BACK, SDL_GAMEPAD_BUTTON_BACK },
};

static void detach(void)
{
	if (joystick)
	{
		SDL_CloseJoystick(joystick);
		joystick = NULL;
	}
	if (joystick_id)
	{
		SDL_DetachVirtualJoystick(joystick_id);
		joystick_id = 0;
	}
}

static bool attach(void)
{
	SDL_VirtualJoystickDesc desc;

	if (joystick)
		return true;
	SDL_INIT_INTERFACE(&desc);
	desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
	desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
	desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
	desc.name = "Touch controls";

	joystick_id = SDL_AttachVirtualJoystick(&desc);
	if (!joystick_id)
	{
		host_logf(HOST_LOG_ERROR, "touch: cannot attach the virtual gamepad: %s", SDL_GetError());
		return false;
	}
	joystick = SDL_OpenJoystick(joystick_id);
	if (!joystick)
	{
		host_logf(HOST_LOG_ERROR, "touch: cannot open the virtual gamepad: %s", SDL_GetError());
		detach();
		return false;
	}
	host_logf(HOST_LOG_INFO, "touch: virtual gamepad attached (id %u)", (unsigned)joystick_id);
	return true;
}

static bool vpad_init(void *userdata)
{
	(void)userdata;
	/* the guest started the gamepad subsystem already; this only counts a use */
	subsystem_started = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
	return subsystem_started;
}

static void vpad_shutdown(void *userdata)
{
	(void)userdata;
	detach();
	if (subsystem_started)
		SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
	subsystem_started = false;
}

static void vpad_set_enabled(void *userdata, bool enabled)
{
	(void)userdata;
	if (enabled)
		attach();
	else
		detach();
}

static Sint16 to_axis(float value)
{
	return (Sint16)(SDL_clamp(value, -1.0f, 1.0f) * 32767.0f);
}

static void vpad_submit(void *userdata, const TC_State *state)
{
	size_t index;

	(void)userdata;
	if (!joystick)
		return;

	SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFTX, to_axis(state->move_x));
	SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFTY, to_axis(state->move_y));

	/* a trigger rests at the axis minimum */
	SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
		(state->buttons & TC_BIT(TC_ACT_FIRE)) ? SDL_JOYSTICK_AXIS_MAX : SDL_JOYSTICK_AXIS_MIN);
	SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER,
		(state->buttons & TC_BIT(TC_ACT_GRENADE)) ? SDL_JOYSTICK_AXIS_MAX : SDL_JOYSTICK_AXIS_MIN);

	for (index = 0; index < SDL_arraysize(button_map); index++)
	{
		SDL_SetJoystickVirtualButton(joystick, (int)button_map[index].button,
			(state->buttons & TC_BIT(button_map[index].action)) != 0);
	}

	/* The virtual driver keeps what was set above until the next
	SDL_UpdateJoysticks, which only the next SDL_PollEvent would run: the
	guest would read last frame's touches. Publish them now. */
	SDL_UpdateJoysticks();
}

const TC_Backend TC_BackendVirtualGamepad =
{
	"virtual gamepad", vpad_init, vpad_shutdown, vpad_set_enabled, vpad_submit
};
