/*
TOUCH_MENU.H

The menus' pointer from a touchscreen: taps and drags of one finger
become what the desktop mouse gives the menus (port/linux/src/sdl_platform.c,
platform_ui_pointer_read). A finger that goes down and up without moving
further than the slop is a tap: the pointer goes where it went down, and
clicks there. A finger that moves further scrolls instead: each step of
its drag along the axis that dominated when it started is one notch of the
wheel, counted from the edge of the slop. No tap and no hover come from a
scroll, so the focus does not follow the finger. Other fingers are
ignored while one is down, and a new finger stops the steps still waiting.

No SDL or game types, so that the build machine can test it
(port/linux/tests/touch_menu_test.c).
*/

#ifndef TOUCH_MENU_H
#define TOUCH_MENU_H

struct touch_menu_settings
{
	/* pixels a tap may move */
	float slop;
	/* pixels of drag for one wheel step */
	float step;
	/* at most this many steps per read: the rest waits, so that a fast
	swipe moves a list gradually, as fast as the menus take the steps */
	int steps_per_read;
	/* Android's system gesture zones, in pixels of the window (all 0: none).
	With the system bars hidden the first edge swipe only shows them, and
	Android hands that swipe to the game as an ordinary finger, so a finger
	that begins in a zone must not act as a menu touch:
	- in the left or right zone it is ignored for its whole life, a corner
	included; it does not keep another finger from being used;
	- in the top or bottom zone it still taps (the A/B legends sit in the
	bottom zone) but never scrolls: once it moves beyond the slop it is
	neither a tap nor a scroll. */
	float edge_left, edge_top, edge_right, edge_bottom;
	/* the window's size, for the right and bottom zones: 0 means those two
	zones do not exist, so that a size not yet known cannot fill the screen.
	A zone with a 0 inset does not exist either. The zones may change
	between two fingers (the phone rotates): they are read at a down. */
	float width, height;
};

struct touch_menu_output
{
	/* where the latest tap was, in pixels */
	float x, y;
	float click_x, click_y;
	/* a tap happened: the pointer is at x, y */
	int moved;
	/* taps since the last read */
	int clicks;
	/* positive: back (the finger moved down or right) */
	int wheel_steps;
	/* fingers that went down since the last read, and where the latest
	did: a drag reports no tap, and the debug view (debug.touch_targets)
	wants to show where it began */
	int downs;
	float down_x, down_y;
};

struct touch_menu
{
	struct touch_menu_settings settings;
	int finger_down;
	unsigned long long finger;
	float down_x, down_y;
	float last_x, last_y;
	int scrolling;
	/* the finger began in the top or bottom zone: it may tap, not scroll */
	int no_scroll;
	/* a no_scroll finger moved beyond the slop: no tap and no scroll */
	int void_tap;
	/* 0: vertical, 1: horizontal */
	int axis;
	/* drag not yet a whole step */
	float remainder;
	int pending_steps;
	struct touch_menu_output output;
};

/**
 * @brief Prepares a menu pointer; the settings are copied.
 * @param menu the state to set up (all of it is cleared)
 * @param settings the gesture sizes, in pixels, and the steps a read
 */
void touch_menu_init(struct touch_menu *menu, const struct touch_menu_settings *settings);

/**
 * @brief Forgets the finger, the pending steps and the unread taps but
 * keeps the settings: what a finger began before a menu opened or closed
 * must not act in the other one.
 * @param menu the state to clear
 */
void touch_menu_reset(struct touch_menu *menu);

/**
 * @brief A finger went down. A second finger is ignored while one is
 * down. The steps still waiting are dropped, so that a tap hits what the
 * finger sees on a list still scrolling. The down is counted and its point
 * kept for the next read (downs, down_x, down_y), even when the finger goes
 * on to scroll.
 * @param menu the state
 * @param finger the finger's id
 * @param x,y where it went down, in pixels
 */
void touch_menu_down(struct touch_menu *menu, unsigned long long finger, float x, float y);

/**
 * @brief The finger moved. Once it is further than the slop it scrolls;
 * the steps count from the slop's edge, so that the slop is not a step
 * and a fast first sample is not lost.
 * @param menu the state
 * @param finger the finger's id; other fingers are ignored
 * @param x,y where it is, in pixels
 */
void touch_menu_move(struct touch_menu *menu, unsigned long long finger, float x, float y);

/**
 * @brief The finger went up: a tap if it never scrolled, at the place
 * where it went down.
 * @param menu the state
 * @param finger the finger's id; other fingers are ignored
 * @param x,y where it went up, in pixels (the last part of a drag counts)
 */
void touch_menu_up(struct touch_menu *menu, unsigned long long finger, float x, float y);

/**
 * @brief The system took the touch away: the gesture neither taps nor
 * scrolls.
 * @param menu the state
 */
void touch_menu_cancel(struct touch_menu *menu);

/**
 * @brief Hands out what happened since the last read. At most
 * steps_per_read wheel steps come out; the rest waits for the next reads.
 * @param menu the state
 * @param output receives the pointer, the taps and the wheel steps
 */
void touch_menu_read(struct touch_menu *menu, struct touch_menu_output *output);

#endif
