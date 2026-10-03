/*
TOUCH_INPUT.C

The touchscreen of the Android build. SDL's finger events become the
menus' pointer (touch_menu.c) while a menu is up, as the desktop mouse
does (sdl_platform.c, platform_ui_pointer_read). Sizes are in dp, from the
display density the app passes (HALO_DISPLAY_DENSITY, host_main.c). The edges
of the screen where Android keeps its gestures are asked of the host at every
finger down (host_gesture_insets).
*/

#include "touch_input.h"
#include "touch_menu.h"

#include <stdlib.h>

#ifdef HALO_ANDROID
/* port/android/guest/runtime/guest_host.h */
void host_gesture_insets(int *insets);
#endif

/* the game's (port/linux/game/touch_game.c) */
int touch_game_cinematic_skippable(void);

/* a tap moves at most this far; a drag of this length is one wheel step */
#define TOUCH_TAP_SLOP_DP 12.0f
#define TOUCH_SCROLL_STEP_DP 40.0f
/* the menus post one press a frame and drop presses beyond their queue
(ui_widget.c): one step a read keeps up with them without losing any */
#define TOUCH_STEPS_PER_READ 1

static struct touch_menu menu;
static int menu_ready;
static int menu_active;
/* polls left to hold A: a tap is shorter than the game's poll, and the
game must see the press */
static int skip_polls;
#define TOUCH_PRESS_POLLS 2

/**
 * @brief The pixels in a dp: Android's densityDpi / 160, or else a guess
 * from the height.
 * @return pixels a dp; 0 while neither is known (the window does not exist
 * yet)
 */
static float pixels_per_dp(void)
{
	const char *density = getenv("HALO_DISPLAY_DENSITY");
	int width = 0, height = 0;

	if (density && atof(density) > 0.0)
		return (float)atof(density);
	platform_video_drawable_size(&width, &height);
	/* a phone held in landscape is about 360 dp high, whatever its density */
	return height > 0 ? (float)height / 360.0f : 0.0f;
}

/**
 * @brief Sets the gestures' sizes up, once they can be known; until then a
 * dp is a pixel, there are no gesture zones and the next call tries again.
 */
static void menu_setup(void)
{
	if (!menu_ready)
	{
		struct touch_menu_settings settings;
		float dp = pixels_per_dp();
		int width = 0, height = 0;

		platform_video_drawable_size(&width, &height);

		settings.slop = TOUCH_TAP_SLOP_DP * (dp > 0.0f ? dp : 1.0f);
		settings.step = TOUCH_SCROLL_STEP_DP * (dp > 0.0f ? dp : 1.0f);
		settings.steps_per_read = TOUCH_STEPS_PER_READ;
		/* no zones until a finger goes down: touch_input_event reads them */
		settings.edge_left = settings.edge_top = settings.edge_right = settings.edge_bottom = 0.0f;
		settings.width = (float)width;
		settings.height = (float)height;
		touch_menu_init(&menu, &settings);
		menu_ready = dp > 0.0f && width > 0 && height > 0;
	}
}

/**
 * @brief Brings the gesture zones and the window's size up to date. It runs
 * at every finger down, not once at startup: the phone rotates after the app
 * has started, and the insets of the portrait screen would stay. The zones
 * matter only at a down, so changing them between two fingers is safe.
 * Without Android's insets (desktop) there are no zones.
 * @param width,height the window's size in pixels
 */
static void refresh_zones(int width, int height)
{
#ifdef HALO_ANDROID
	int insets[4];

	host_gesture_insets(insets);
	menu.settings.edge_left = (float)insets[0];
	menu.settings.edge_top = (float)insets[1];
	menu.settings.edge_right = (float)insets[2];
	menu.settings.edge_bottom = (float)insets[3];
#endif
	menu.settings.width = (float)width;
	menu.settings.height = (float)height;
}

void touch_input_event(unsigned int type, const SDL_TouchFingerEvent *finger)
{
	int width = 0, height = 0;
	float x, y;

	menu_setup();
	/* SDL gives 0..1 of the window; the window is its pixels on Android */
	platform_video_drawable_size(&width, &height);
	x = finger->x * (float)width;
	y = finger->y * (float)height;
	switch (type)
	{
	case SDL_EVENT_FINGER_DOWN:
		refresh_zones(width, height);
		touch_menu_down(&menu, finger->fingerID, x, y);
		break;
	case SDL_EVENT_FINGER_MOTION:
		touch_menu_move(&menu, finger->fingerID, x, y);
		break;
	case SDL_EVENT_FINGER_UP:
		touch_menu_up(&menu, finger->fingerID, x, y);
		break;
	case SDL_EVENT_FINGER_CANCELED:
		touch_menu_cancel(&menu);
		break;
	}
}

void touch_input_cancel(void)
{
	menu_setup();
	touch_menu_cancel(&menu);
}

void touch_input_menu_set_active(int active)
{
	/* what a finger began before the menu opened or closed must not
	click in it */
	menu_setup();
	touch_menu_reset(&menu);
	menu_active = active;
}

void touch_input_menu_read(struct platform_ui_pointer *pointer)
{
	struct touch_menu_output output;

	menu_setup();
	touch_menu_read(&menu, &output);
	pointer->x = output.x;
	pointer->y = output.y;
	pointer->click_x = output.click_x;
	pointer->click_y = output.click_y;
	pointer->moved = output.moved ? TRUE : FALSE;
	pointer->left_clicks = output.clicks;
	pointer->right_clicks = 0;
	pointer->wheel_steps = output.wheel_steps;
	pointer->downs = output.downs;
	pointer->down_x = output.down_x;
	pointer->down_y = output.down_y;
	pointer->touch = TRUE;
}

void touch_input_gamepad(XINPUT_GAMEPAD *pad)
{
	/* outside the menus the touchscreen only skips a cinematic that can be
	skipped: a tap there presses A */
	if (!menu_active)
	{
		struct touch_menu_output output;

		menu_setup();
		touch_menu_read(&menu, &output);
		if (output.clicks && touch_game_cinematic_skippable())
		{
			skip_polls = TOUCH_PRESS_POLLS;
			platform_log("touch: A pressed to skip a cinematic");
		}
	}
	if (skip_polls > 0)
	{
		pad->bAnalogButtons[XINPUT_GAMEPAD_A] = 0xff;
		skip_polls--;
	}
}
