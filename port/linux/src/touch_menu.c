/*
TOUCH_MENU.C

The menus' pointer from a touchscreen (touch_menu.h).
*/

#include "touch_menu.h"

#include <math.h>
#include <string.h>

void touch_menu_init(struct touch_menu *menu, const struct touch_menu_settings *settings)
{
	memset(menu, 0, sizeof(*menu));
	menu->settings = *settings;
}

void touch_menu_reset(struct touch_menu *menu)
{
	struct touch_menu_settings settings = menu->settings;

	touch_menu_init(menu, &settings);
}

/**
 * @brief Whether x is in the left or right gesture zone: [0, edge_left) and
 * [width - edge_right, width). A zone with no inset does not exist (x at the
 * window's edge is not in it), and neither does the right one without a
 * window size: it would start at a negative x and cover the screen.
 * @param menu the state (the zones are in its settings)
 * @param x the finger's x, in pixels
 */
static int in_side_zone(const struct touch_menu *menu, float x)
{
	const struct touch_menu_settings *s = &menu->settings;

	return x < s->edge_left || (s->edge_right > 0.0f && s->width > 0.0f && x >= s->width - s->edge_right);
}

/**
 * @brief Whether y is in the top or bottom gesture zone; as the side zones,
 * the bottom one needs an inset and the window's height.
 * @param menu the state (the zones are in its settings)
 * @param y the finger's y, in pixels
 */
static int in_top_or_bottom_zone(const struct touch_menu *menu, float y)
{
	const struct touch_menu_settings *s = &menu->settings;

	return y < s->edge_top || (s->edge_bottom > 0.0f && s->height > 0.0f && y >= s->height - s->edge_bottom);
}

void touch_menu_down(struct touch_menu *menu, unsigned long long finger, float x, float y)
{
	if (menu->finger_down)
		return;
	/* no state for an ignored finger: its moves and its lift find no finger
	down, or another finger's id, and do nothing. It must not set
	finger_down, or it would shut out the next finger. */
	if (in_side_zone(menu, x))
		return;
	menu->finger_down = 1;
	menu->finger = finger;
	menu->down_x = menu->last_x = x;
	menu->down_y = menu->last_y = y;
	menu->output.downs++;
	menu->output.down_x = x;
	menu->output.down_y = y;
	menu->scrolling = 0;
	menu->no_scroll = in_top_or_bottom_zone(menu, y);
	menu->void_tap = 0;
	menu->remainder = 0.0f;
	/* a finger on the screen stops a list still scrolling, so that a tap
	hits what the finger sees */
	menu->pending_steps = 0;
}

void touch_menu_move(struct touch_menu *menu, unsigned long long finger, float x, float y)
{
	float along;

	if (!menu->finger_down || finger != menu->finger)
		return;
	if (!menu->scrolling)
	{
		float dx = x - menu->down_x, dy = y - menu->down_y;

		if (dx * dx + dy * dy <= menu->settings.slop * menu->settings.slop)
			return;
		/* a finger from the top or bottom zone may be Android's own edge swipe,
		which reaches the game too. It may tap (the legends sit in the bottom
		zone), but beyond the slop it is neither a scroll nor a tap, so that
		its lift does not click where it began. */
		if (menu->no_scroll)
		{
			menu->void_tap = 1;
			return;
		}
		/* the steps count from the slop's edge on the axis: the slop is not
		a step, and a fast first sample is not lost */
		menu->scrolling = 1;
		menu->axis = fabsf(dx) > fabsf(dy);
		if (menu->axis)
			menu->last_x = menu->down_x + (dx > 0.0f ? menu->settings.slop : -menu->settings.slop);
		else
			menu->last_y = menu->down_y + (dy > 0.0f ? menu->settings.slop : -menu->settings.slop);
	}
	along = menu->axis ? x - menu->last_x : y - menu->last_y;
	menu->last_x = x;
	menu->last_y = y;
	menu->remainder += along;
	while (menu->remainder >= menu->settings.step)
	{
		menu->pending_steps++;
		menu->remainder -= menu->settings.step;
	}
	while (menu->remainder <= -menu->settings.step)
	{
		menu->pending_steps--;
		menu->remainder += menu->settings.step;
	}
}

void touch_menu_up(struct touch_menu *menu, unsigned long long finger, float x, float y)
{
	if (!menu->finger_down || finger != menu->finger)
		return;
	touch_menu_move(menu, finger, x, y);
	if (!menu->scrolling && !menu->void_tap)
	{
		menu->output.moved = 1;
		menu->output.x = menu->output.click_x = menu->down_x;
		menu->output.y = menu->output.click_y = menu->down_y;
		menu->output.clicks++;
	}
	menu->finger_down = 0;
	menu->scrolling = 0;
	menu->void_tap = 0;
}

void touch_menu_cancel(struct touch_menu *menu)
{
	menu->finger_down = 0;
	menu->scrolling = 0;
	menu->void_tap = 0;
	menu->remainder = 0.0f;
	menu->pending_steps = 0;
	menu->output.downs = 0;
}

void touch_menu_read(struct touch_menu *menu, struct touch_menu_output *output)
{
	int limit = menu->settings.steps_per_read;
	int steps = menu->pending_steps;

	if (steps > limit)
		steps = limit;
	if (steps < -limit)
		steps = -limit;
	*output = menu->output;
	output->wheel_steps = steps;
	menu->pending_steps -= steps;
	menu->output.moved = 0;
	menu->output.clicks = 0;
	menu->output.downs = 0;
}
