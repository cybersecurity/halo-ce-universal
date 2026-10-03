/*
TOUCH_MENU_TEST.C

Tests of the menus' touch gestures (port/linux/src/touch_menu.c). Built and
run by tools/test_touch_menu.py; exits nonzero on a failure.
*/

#include "../src/touch_menu.h"

#include <stdio.h>

static int failures;

#define CHECK(condition) \
	do \
	{ \
		if (!(condition)) \
		{ \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
			failures++; \
		} \
	} while (0)

/* 3 pixels a dp, as on the S23: slop 12 dp, step 40 dp; 2 steps a read, to
test the limit (the game uses 1) */
static void setup(struct touch_menu *menu)
{
	struct touch_menu_settings settings = { 36.0f, 120.0f, 2, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };

	touch_menu_init(menu, &settings);
}

/* the S23's system gesture insets, as dumpsys window reports them, on its
2340x1080 window: 90 px at the sides, 40 above (the status bar), 96 below */
static void setup_zones(struct touch_menu *menu)
{
	struct touch_menu_settings settings = { 36.0f, 120.0f, 2, 90.0f, 40.0f, 90.0f, 96.0f, 2340.0f, 1080.0f };

	touch_menu_init(menu, &settings);
}

/* a finger from (x0, y0) to (x1, y1) in 12 samples, lifted at the end */
static void swipe(struct touch_menu *menu, unsigned long long finger, float x0, float y0, float x1, float y1)
{
	int i;

	touch_menu_down(menu, finger, x0, y0);
	for (i = 1; i <= 12; i++)
		touch_menu_move(menu, finger, x0 + (x1 - x0) * i / 12.0f, y0 + (y1 - y0) * i / 12.0f);
	touch_menu_up(menu, finger, x1, y1);
}

/* first, so that a unit that lost its settings (a step of 0 never ends its
loops) fails here and not by hanging a later test */
static void test_init_stores_the_settings(void)
{
	struct touch_menu menu;

	setup(&menu);
	CHECK(menu.settings.slop == 36.0f);
	CHECK(menu.settings.step == 120.0f);
	CHECK(menu.settings.steps_per_read == 2);
	CHECK(menu.settings.edge_left == 0.0f && menu.settings.edge_top == 0.0f);
	CHECK(menu.settings.edge_right == 0.0f && menu.settings.edge_bottom == 0.0f);
	CHECK(menu.settings.width == 0.0f && menu.settings.height == 0.0f);
	setup_zones(&menu);
	CHECK(menu.settings.edge_left == 90.0f);
	CHECK(menu.settings.edge_top == 40.0f);
	CHECK(menu.settings.edge_right == 90.0f);
	CHECK(menu.settings.edge_bottom == 96.0f);
	CHECK(menu.settings.width == 2340.0f);
	CHECK(menu.settings.height == 1080.0f);
}

static void test_tap_clicks_where_the_finger_went_down(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup(&menu);
	touch_menu_down(&menu, 7, 500.0f, 300.0f);
	touch_menu_move(&menu, 7, 510.0f, 305.0f);
	touch_menu_up(&menu, 7, 512.0f, 306.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.moved == 1);
	CHECK(out.clicks == 1);
	CHECK(out.x == 500.0f && out.y == 300.0f);
	CHECK(out.click_x == 500.0f && out.click_y == 300.0f);
	CHECK(out.wheel_steps == 0);
}

static void test_read_clears_the_tap(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup(&menu);
	touch_menu_down(&menu, 1, 10.0f, 10.0f);
	touch_menu_up(&menu, 1, 10.0f, 10.0f);
	touch_menu_read(&menu, &out);
	touch_menu_read(&menu, &out);
	CHECK(out.moved == 0);
	CHECK(out.clicks == 0);
}

static void test_nothing_until_the_finger_lifts(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup(&menu);
	touch_menu_down(&menu, 1, 10.0f, 10.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.moved == 0);
	CHECK(out.clicks == 0);
}

static void test_a_tap_may_move_the_whole_slop(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup(&menu);
	touch_menu_down(&menu, 1, 100.0f, 100.0f);
	touch_menu_up(&menu, 1, 136.0f, 100.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.clicks == 1);
}

static void test_two_taps_between_reads(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup(&menu);
	touch_menu_down(&menu, 1, 10.0f, 10.0f);
	touch_menu_up(&menu, 1, 10.0f, 10.0f);
	touch_menu_down(&menu, 2, 70.0f, 80.0f);
	touch_menu_up(&menu, 2, 70.0f, 80.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.clicks == 2);
	CHECK(out.click_x == 70.0f && out.click_y == 80.0f);
}

static void test_drag_down_scrolls_back_without_clicking(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup(&menu);
	touch_menu_down(&menu, 1, 500.0f, 100.0f);
	/* past the slop (counted from 136), then 250 further: 2 steps */
	touch_menu_move(&menu, 1, 500.0f, 140.0f);
	touch_menu_move(&menu, 1, 500.0f, 390.0f);
	touch_menu_up(&menu, 1, 500.0f, 390.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.wheel_steps == 2);
	CHECK(out.clicks == 0);
	CHECK(out.moved == 0);
}

static void test_drag_up_scrolls_forward(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup(&menu);
	touch_menu_down(&menu, 1, 500.0f, 800.0f);
	touch_menu_move(&menu, 1, 500.0f, 760.0f);
	touch_menu_move(&menu, 1, 500.0f, 630.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.wheel_steps == -1);
}

static void test_horizontal_drag_uses_the_x_axis(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup(&menu);
	touch_menu_down(&menu, 1, 1500.0f, 500.0f);
	/* mostly left: forward */
	touch_menu_move(&menu, 1, 1460.0f, 510.0f);
	touch_menu_move(&menu, 1, 1330.0f, 540.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.wheel_steps == -1);
}

static void test_equal_dx_dy_scrolls_vertically(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup(&menu);
	touch_menu_down(&menu, 1, 500.0f, 500.0f);
	touch_menu_move(&menu, 1, 530.0f, 530.0f);
	touch_menu_move(&menu, 1, 530.0f, 680.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.wheel_steps == 1);
}

static void test_the_axis_stays_after_the_start(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup(&menu);
	touch_menu_down(&menu, 1, 500.0f, 500.0f);
	touch_menu_move(&menu, 1, 500.0f, 540.0f);
	touch_menu_move(&menu, 1, 900.0f, 540.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.wheel_steps == 0);
}

static void test_a_fast_drag_counts_from_the_slop(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;
	int total = 0, index;

	setup(&menu);
	touch_menu_down(&menu, 1, 500.0f, 0.0f);
	/* one sample far past the slop, as a flick or a slow frame gives */
	touch_menu_move(&menu, 1, 500.0f, 600.0f);
	touch_menu_up(&menu, 1, 500.0f, 600.0f);
	for (index = 0; index < 5; index++)
	{
		touch_menu_read(&menu, &out);
		total += out.wheel_steps;
	}
	/* (600 - 36) / 120 = 4.7 */
	CHECK(total == 4);
	CHECK(out.clicks == 0);
}

static void test_steps_come_out_gradually(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup(&menu);
	touch_menu_down(&menu, 1, 500.0f, 0.0f);
	touch_menu_move(&menu, 1, 500.0f, 40.0f);
	/* five steps at once */
	touch_menu_move(&menu, 1, 500.0f, 640.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.wheel_steps == 2);
	touch_menu_read(&menu, &out);
	CHECK(out.wheel_steps == 2);
	touch_menu_read(&menu, &out);
	CHECK(out.wheel_steps == 1);
	touch_menu_read(&menu, &out);
	CHECK(out.wheel_steps == 0);
}

static void test_forward_steps_come_out_gradually(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup(&menu);
	touch_menu_down(&menu, 1, 500.0f, 1000.0f);
	touch_menu_move(&menu, 1, 500.0f, 960.0f);
	touch_menu_move(&menu, 1, 500.0f, 360.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.wheel_steps == -2);
	touch_menu_read(&menu, &out);
	CHECK(out.wheel_steps == -2);
	touch_menu_read(&menu, &out);
	CHECK(out.wheel_steps == -1);
	touch_menu_read(&menu, &out);
	CHECK(out.wheel_steps == 0);
}

static void test_part_of_a_step_is_kept(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup(&menu);
	touch_menu_down(&menu, 1, 500.0f, 0.0f);
	touch_menu_move(&menu, 1, 500.0f, 40.0f);
	touch_menu_move(&menu, 1, 500.0f, 100.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.wheel_steps == 0);
	touch_menu_move(&menu, 1, 500.0f, 160.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.wheel_steps == 1);
}

static void test_lift_far_away_is_no_tap(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup(&menu);
	touch_menu_down(&menu, 1, 100.0f, 100.0f);
	touch_menu_up(&menu, 1, 100.0f, 600.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.clicks == 0);
	CHECK(out.moved == 0);
	/* the lift's own move scrolls: (600 - 100 - 36) / 120 = 3.9 steps, 2 a read */
	CHECK(out.wheel_steps == 2);
}

static void test_a_scroll_keeps_the_last_tap_point_without_moving(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup(&menu);
	touch_menu_down(&menu, 1, 10.0f, 20.0f);
	touch_menu_up(&menu, 1, 10.0f, 20.0f);
	touch_menu_read(&menu, &out);
	touch_menu_down(&menu, 2, 500.0f, 0.0f);
	touch_menu_move(&menu, 2, 500.0f, 40.0f);
	touch_menu_move(&menu, 2, 500.0f, 200.0f);
	touch_menu_up(&menu, 2, 500.0f, 200.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.moved == 0);
	CHECK(out.x == 10.0f && out.y == 20.0f);
}

static void test_a_new_finger_stops_pending_steps(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup(&menu);
	touch_menu_down(&menu, 1, 500.0f, 0.0f);
	touch_menu_move(&menu, 1, 500.0f, 40.0f);
	touch_menu_move(&menu, 1, 500.0f, 640.0f);
	touch_menu_up(&menu, 1, 500.0f, 640.0f);
	/* the tap hits the list as the finger saw it */
	touch_menu_down(&menu, 2, 300.0f, 300.0f);
	touch_menu_up(&menu, 2, 300.0f, 300.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.clicks == 1);
	CHECK(out.wheel_steps == 0);
}

static void test_second_finger_is_ignored(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup(&menu);
	touch_menu_down(&menu, 1, 100.0f, 100.0f);
	touch_menu_down(&menu, 2, 900.0f, 900.0f);
	touch_menu_move(&menu, 2, 900.0f, 100.0f);
	touch_menu_up(&menu, 2, 900.0f, 100.0f);
	touch_menu_up(&menu, 1, 100.0f, 100.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.clicks == 1);
	CHECK(out.x == 100.0f && out.y == 100.0f);
	CHECK(out.wheel_steps == 0);
}

static void test_lift_of_another_finger_is_ignored(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup(&menu);
	touch_menu_down(&menu, 1, 100.0f, 100.0f);
	touch_menu_up(&menu, 9, 100.0f, 100.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.clicks == 0 && out.moved == 0);
	touch_menu_up(&menu, 1, 100.0f, 100.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.clicks == 1);
}

static void test_move_and_lift_without_down_do_nothing(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup(&menu);
	touch_menu_move(&menu, 1, 100.0f, 900.0f);
	touch_menu_up(&menu, 1, 100.0f, 900.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.clicks == 0 && out.moved == 0 && out.wheel_steps == 0);
}

static void test_cancel_drops_the_gesture(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup(&menu);
	touch_menu_down(&menu, 1, 500.0f, 0.0f);
	touch_menu_move(&menu, 1, 500.0f, 40.0f);
	touch_menu_move(&menu, 1, 500.0f, 400.0f);
	touch_menu_cancel(&menu);
	touch_menu_up(&menu, 1, 500.0f, 400.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.wheel_steps == 0);
	CHECK(out.clicks == 0);
	/* and the next finger works */
	touch_menu_down(&menu, 3, 20.0f, 20.0f);
	touch_menu_up(&menu, 3, 20.0f, 20.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.clicks == 1);
}

static void test_reset_clears_everything(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup(&menu);
	touch_menu_down(&menu, 1, 10.0f, 10.0f);
	touch_menu_up(&menu, 1, 10.0f, 10.0f);
	touch_menu_reset(&menu);
	touch_menu_read(&menu, &out);
	CHECK(out.clicks == 0);
	CHECK(out.moved == 0);
}

static void test_reset_during_a_touch_ignores_its_lift(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup(&menu);
	touch_menu_down(&menu, 1, 100.0f, 100.0f);
	touch_menu_reset(&menu);
	touch_menu_move(&menu, 1, 100.0f, 500.0f);
	touch_menu_up(&menu, 1, 100.0f, 500.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.clicks == 0 && out.wheel_steps == 0);
}

/* a step is exactly the step setting: 120 pixels, not 119.9 or 120.1 */
static void test_a_step_is_exactly_the_step_setting(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup(&menu);
	touch_menu_down(&menu, 1, 500.0f, 0.0f);
	touch_menu_move(&menu, 1, 500.0f, 40.0f);
	touch_menu_move(&menu, 1, 500.0f, 156.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.wheel_steps == 1);
	touch_menu_cancel(&menu);
	touch_menu_down(&menu, 2, 500.0f, 1000.0f);
	touch_menu_move(&menu, 2, 500.0f, 960.0f);
	touch_menu_move(&menu, 2, 500.0f, 844.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.wheel_steps == -1);
}

static void test_horizontal_drag_right_scrolls_back(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup(&menu);
	touch_menu_down(&menu, 1, 100.0f, 500.0f);
	/* no vertical movement at all: the distance is all in x */
	touch_menu_move(&menu, 1, 140.0f, 500.0f);
	touch_menu_move(&menu, 1, 180.0f, 500.0f);
	touch_menu_move(&menu, 1, 245.0f, 500.0f);
	touch_menu_read(&menu, &out);
	/* 245 - 136 = 109 of the 120 */
	CHECK(out.wheel_steps == 0);
	touch_menu_move(&menu, 1, 257.0f, 500.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.wheel_steps == 1);
	CHECK(out.clicks == 0);
}

static void test_a_new_finger_starts_a_fresh_step(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup(&menu);
	touch_menu_down(&menu, 1, 500.0f, 0.0f);
	touch_menu_move(&menu, 1, 500.0f, 40.0f);
	touch_menu_move(&menu, 1, 500.0f, 100.0f);
	touch_menu_up(&menu, 1, 500.0f, 100.0f);
	/* the same 64 pixels again: 128 together would be a step */
	touch_menu_down(&menu, 2, 500.0f, 0.0f);
	touch_menu_move(&menu, 2, 500.0f, 40.0f);
	touch_menu_move(&menu, 2, 500.0f, 100.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.wheel_steps == 0);
}

static void test_a_lifted_finger_cannot_move_again(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup(&menu);
	touch_menu_down(&menu, 1, 100.0f, 100.0f);
	touch_menu_up(&menu, 1, 100.0f, 100.0f);
	touch_menu_read(&menu, &out);
	/* the same id, no finger down */
	touch_menu_move(&menu, 1, 100.0f, 900.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.wheel_steps == 0);
}

static void test_reset_keeps_the_settings(void)
{
	struct touch_menu menu;

	setup(&menu);
	touch_menu_reset(&menu);
	CHECK(menu.settings.slop == 36.0f);
	CHECK(menu.settings.step == 120.0f);
	CHECK(menu.settings.steps_per_read == 2);
}

static void test_cancel_keeps_a_finished_tap(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup(&menu);
	touch_menu_down(&menu, 1, 30.0f, 40.0f);
	touch_menu_up(&menu, 1, 30.0f, 40.0f);
	touch_menu_cancel(&menu);
	touch_menu_read(&menu, &out);
	CHECK(out.clicks == 1);
	CHECK(out.moved == 1);
	CHECK(out.click_x == 30.0f && out.click_y == 40.0f);
}

/* the game's value: one step a read */
static void test_one_step_a_read(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;
	struct touch_menu_settings settings = { 36.0f, 120.0f, 1, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
	int index;

	touch_menu_init(&menu, &settings);
	touch_menu_down(&menu, 1, 500.0f, 0.0f);
	touch_menu_move(&menu, 1, 500.0f, 40.0f);
	touch_menu_move(&menu, 1, 500.0f, 640.0f);
	for (index = 0; index < 5; index++)
	{
		touch_menu_read(&menu, &out);
		CHECK(out.wheel_steps == 1);
	}
	touch_menu_read(&menu, &out);
	CHECK(out.wheel_steps == 0);
	touch_menu_cancel(&menu);
	touch_menu_down(&menu, 2, 500.0f, 1000.0f);
	touch_menu_move(&menu, 2, 500.0f, 960.0f);
	touch_menu_move(&menu, 2, 500.0f, 360.0f);
	for (index = 0; index < 5; index++)
	{
		touch_menu_read(&menu, &out);
		CHECK(out.wheel_steps == -1);
	}
	touch_menu_read(&menu, &out);
	CHECK(out.wheel_steps == 0);
}

/* the debug view (debug.touch_targets) shows where a drag began, which no
tap or hover reports */
static void test_a_drag_reports_where_the_finger_went_down(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup(&menu);
	touch_menu_down(&menu, 3, 400.0f, 200.0f);
	touch_menu_move(&menu, 3, 400.0f, 500.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.downs == 1);
	CHECK(out.down_x == 400.0f && out.down_y == 200.0f);
	CHECK(out.clicks == 0);
	touch_menu_read(&menu, &out);
	CHECK(out.downs == 0);
}

static void test_a_tap_reports_its_down(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup(&menu);
	touch_menu_down(&menu, 1, 50.0f, 60.0f);
	touch_menu_up(&menu, 1, 50.0f, 60.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.downs == 1);
	CHECK(out.down_x == 50.0f && out.down_y == 60.0f);
}

static void test_two_downs_between_reads_count_twice(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup(&menu);
	touch_menu_down(&menu, 1, 50.0f, 60.0f);
	touch_menu_up(&menu, 1, 50.0f, 60.0f);
	touch_menu_down(&menu, 2, 70.0f, 80.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.downs == 2);
	CHECK(out.down_x == 70.0f && out.down_y == 80.0f);
}

/* an ignored second finger is not a down */
static void test_a_second_finger_is_no_down(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup(&menu);
	touch_menu_down(&menu, 1, 50.0f, 60.0f);
	touch_menu_down(&menu, 2, 70.0f, 80.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.downs == 1);
	CHECK(out.down_x == 50.0f && out.down_y == 60.0f);
}

static void test_reset_and_cancel_drop_the_unread_downs(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup(&menu);
	touch_menu_down(&menu, 1, 50.0f, 60.0f);
	touch_menu_reset(&menu);
	touch_menu_read(&menu, &out);
	CHECK(out.downs == 0);
	touch_menu_down(&menu, 1, 50.0f, 60.0f);
	touch_menu_cancel(&menu);
	touch_menu_read(&menu, &out);
	CHECK(out.downs == 0);
}

static void test_a_tap_in_the_left_zone_is_ignored(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup_zones(&menu);
	touch_menu_down(&menu, 1, 10.0f, 500.0f);
	touch_menu_up(&menu, 1, 10.0f, 500.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.clicks == 0);
	CHECK(out.moved == 0);
	CHECK(out.downs == 0);
	CHECK(out.wheel_steps == 0);
}

static void test_the_left_zone_ends_at_the_inset(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup_zones(&menu);
	touch_menu_down(&menu, 1, 89.0f, 500.0f);
	touch_menu_up(&menu, 1, 89.0f, 500.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.clicks == 0);
	CHECK(out.downs == 0);
	touch_menu_down(&menu, 1, 90.0f, 500.0f);
	touch_menu_up(&menu, 1, 90.0f, 500.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.clicks == 1);
	CHECK(out.downs == 1);
	CHECK(out.click_x == 90.0f);
}

static void test_the_right_zone_begins_at_width_minus_the_inset(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup_zones(&menu);
	touch_menu_down(&menu, 1, 2250.0f, 500.0f);
	touch_menu_up(&menu, 1, 2250.0f, 500.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.clicks == 0);
	CHECK(out.downs == 0);
	touch_menu_down(&menu, 1, 2249.0f, 500.0f);
	touch_menu_up(&menu, 1, 2249.0f, 500.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.clicks == 1);
	CHECK(out.downs == 1);
	CHECK(out.click_x == 2249.0f);
}

/* the reported bug: the first edge swipe in immersive mode scrolled the list */
static void test_an_edge_swipe_does_not_scroll_or_click(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;
	int i, steps = 0;

	setup_zones(&menu);
	touch_menu_down(&menu, 1, 10.0f, 500.0f);
	for (i = 1; i <= 14; i++)
	{
		touch_menu_move(&menu, 1, 10.0f + 50.0f * i, 500.0f);
		touch_menu_read(&menu, &out);
		steps += out.wheel_steps;
		CHECK(out.clicks == 0);
	}
	touch_menu_up(&menu, 1, 710.0f, 500.0f);
	touch_menu_read(&menu, &out);
	steps += out.wheel_steps;
	CHECK(steps == 0);
	CHECK(out.clicks == 0);
	CHECK(out.moved == 0);
	CHECK(out.downs == 0);
}

static void test_an_ignored_finger_does_not_stick(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup_zones(&menu);
	swipe(&menu, 1, 10.0f, 500.0f, 700.0f, 500.0f);
	touch_menu_read(&menu, &out);
	touch_menu_down(&menu, 1, 1170.0f, 540.0f);
	touch_menu_up(&menu, 1, 1170.0f, 540.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.clicks == 1);
	CHECK(out.downs == 1);
}

static void test_an_edge_finger_does_not_block_a_second_finger(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup_zones(&menu);
	touch_menu_down(&menu, 1, 10.0f, 500.0f);
	touch_menu_down(&menu, 2, 1170.0f, 540.0f);
	touch_menu_up(&menu, 2, 1170.0f, 540.0f);
	touch_menu_up(&menu, 1, 10.0f, 500.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.clicks == 1);
	CHECK(out.downs == 1);
	CHECK(out.click_x == 1170.0f);
}

/* an edge finger must not stop the steps of a scroll by another finger */
static void test_an_edge_finger_leaves_another_fingers_scroll_alone(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup_zones(&menu);
	touch_menu_down(&menu, 2, 1170.0f, 800.0f);
	touch_menu_move(&menu, 2, 1170.0f, 500.0f);
	touch_menu_down(&menu, 1, 10.0f, 500.0f);
	touch_menu_move(&menu, 1, 400.0f, 500.0f);
	touch_menu_up(&menu, 1, 400.0f, 500.0f);
	touch_menu_up(&menu, 2, 1170.0f, 500.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.wheel_steps == -2);
	CHECK(out.clicks == 0);
}

/* a finger in a zone is no finger on the screen: the list goes on scrolling */
static void test_an_edge_finger_keeps_the_pending_steps(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup_zones(&menu);
	swipe(&menu, 1, 1170.0f, 800.0f, 1170.0f, 100.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.wheel_steps == -2);
	touch_menu_down(&menu, 2, 10.0f, 500.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.wheel_steps == -2);
}

static void test_a_tap_in_the_bottom_zone_clicks(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup_zones(&menu);
	touch_menu_down(&menu, 1, 1170.0f, 1050.0f);
	touch_menu_up(&menu, 1, 1170.0f, 1050.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.clicks == 1);
	CHECK(out.downs == 1);
	CHECK(out.click_x == 1170.0f && out.click_y == 1050.0f);
}

static void test_a_swipe_up_from_the_bottom_zone_does_nothing(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;
	int i, steps = 0;

	setup_zones(&menu);
	touch_menu_down(&menu, 1, 1170.0f, 1060.0f);
	for (i = 1; i <= 12; i++)
	{
		touch_menu_move(&menu, 1, 1170.0f, 1060.0f - 50.0f * i);
		touch_menu_read(&menu, &out);
		steps += out.wheel_steps;
	}
	touch_menu_up(&menu, 1, 1170.0f, 460.0f);
	touch_menu_read(&menu, &out);
	steps += out.wheel_steps;
	CHECK(steps == 0);
	CHECK(out.clicks == 0);
	CHECK(out.moved == 0);
}

static void test_a_swipe_down_from_the_top_zone_does_nothing(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;
	int i, steps = 0;

	setup_zones(&menu);
	touch_menu_down(&menu, 1, 1170.0f, 10.0f);
	for (i = 1; i <= 12; i++)
	{
		touch_menu_move(&menu, 1, 1170.0f, 10.0f + 50.0f * i);
		touch_menu_read(&menu, &out);
		steps += out.wheel_steps;
	}
	touch_menu_up(&menu, 1, 1170.0f, 610.0f);
	touch_menu_read(&menu, &out);
	steps += out.wheel_steps;
	CHECK(steps == 0);
	CHECK(out.clicks == 0);
	CHECK(out.moved == 0);
}

/* a sample past the slop voids the tap for good: coming back to the start
must not bring it back */
static void test_a_no_scroll_finger_stays_void_after_returning(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup_zones(&menu);
	touch_menu_down(&menu, 1, 1170.0f, 1050.0f);
	touch_menu_move(&menu, 1, 1170.0f, 900.0f);
	touch_menu_up(&menu, 1, 1170.0f, 1050.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.clicks == 0);
	CHECK(out.moved == 0);
}

/* a no-scroll finger may still wobble inside the slop and tap */
static void test_a_no_scroll_finger_taps_within_the_slop(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup_zones(&menu);
	touch_menu_down(&menu, 1, 1170.0f, 1050.0f);
	touch_menu_move(&menu, 1, 1190.0f, 1040.0f);
	touch_menu_up(&menu, 1, 1190.0f, 1040.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.clicks == 1);
	CHECK(out.click_x == 1170.0f && out.click_y == 1050.0f);
}

static void test_just_outside_the_zones_scrolls_as_usual(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup_zones(&menu);
	touch_menu_down(&menu, 1, 1170.0f, 900.0f);
	touch_menu_move(&menu, 1, 1170.0f, 840.0f);
	touch_menu_move(&menu, 1, 1170.0f, 300.0f);
	touch_menu_up(&menu, 1, 1170.0f, 300.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.wheel_steps == -2);
	CHECK(out.clicks == 0);
}

static void test_the_bottom_and_top_zones_end_at_their_insets(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	/* 1080 - 96 = 984 is the first y of the bottom zone */
	setup_zones(&menu);
	swipe(&menu, 1, 1170.0f, 984.0f, 1170.0f, 300.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.wheel_steps == 0);
	swipe(&menu, 1, 1170.0f, 983.0f, 1170.0f, 300.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.wheel_steps == -2);
	/* 39 is the last y of the top zone */
	setup_zones(&menu);
	swipe(&menu, 1, 1170.0f, 39.0f, 1170.0f, 700.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.wheel_steps == 0);
	swipe(&menu, 1, 1170.0f, 40.0f, 1170.0f, 700.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.wheel_steps == 2);
}

/* a no-scroll finger must not leave its state to the next finger */
static void test_a_scroll_after_a_no_scroll_finger_still_scrolls(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup_zones(&menu);
	swipe(&menu, 1, 1170.0f, 1060.0f, 1170.0f, 700.0f);
	touch_menu_read(&menu, &out);
	swipe(&menu, 1, 1170.0f, 800.0f, 1170.0f, 300.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.wheel_steps == -2);
	CHECK(out.clicks == 0);
}

/* the side zones win over the bottom: a corner touch is ignored */
static void test_a_corner_touch_is_ignored(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup_zones(&menu);
	touch_menu_down(&menu, 1, 10.0f, 1060.0f);
	touch_menu_up(&menu, 1, 10.0f, 1060.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.clicks == 0);
	CHECK(out.downs == 0);
	touch_menu_down(&menu, 1, 2300.0f, 10.0f);
	touch_menu_up(&menu, 1, 2300.0f, 10.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.clicks == 0);
	CHECK(out.downs == 0);
}

/* no insets (button navigation, desktop, unreadable): the edges count */
static void test_without_zones_the_edges_work(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup(&menu);
	touch_menu_down(&menu, 1, 0.0f, 500.0f);
	touch_menu_up(&menu, 1, 0.0f, 500.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.clicks == 1);
	touch_menu_down(&menu, 1, 500.0f, 0.0f);
	touch_menu_up(&menu, 1, 500.0f, 0.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.clicks == 1);
	swipe(&menu, 1, 0.0f, 500.0f, 500.0f, 500.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.wheel_steps == 2);
	CHECK(out.clicks == 0);
}

/* insets without a window size: the right and bottom zones do not exist */
static void test_without_a_size_only_the_left_and_top_zones_exist(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;
	struct touch_menu_settings settings = { 36.0f, 120.0f, 2, 90.0f, 40.0f, 90.0f, 96.0f, 0.0f, 0.0f };

	touch_menu_init(&menu, &settings);
	touch_menu_down(&menu, 1, 10.0f, 500.0f);
	touch_menu_up(&menu, 1, 10.0f, 500.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.clicks == 0);
	swipe(&menu, 1, 5000.0f, 2000.0f, 5000.0f, 1000.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.wheel_steps == -2);
}

/* the S23's insets are symmetric, so only unequal ones tell the two sides
(and the top and the bottom) apart */
static void test_each_side_has_its_own_inset(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;
	struct touch_menu_settings settings = { 36.0f, 120.0f, 2, 30.0f, 20.0f, 200.0f, 150.0f, 2340.0f, 1080.0f };

	touch_menu_init(&menu, &settings);
	touch_menu_down(&menu, 1, 29.0f, 500.0f);
	touch_menu_up(&menu, 1, 29.0f, 500.0f);
	touch_menu_down(&menu, 1, 2140.0f, 500.0f);
	touch_menu_up(&menu, 1, 2140.0f, 500.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.clicks == 0);
	touch_menu_down(&menu, 1, 30.0f, 500.0f);
	touch_menu_up(&menu, 1, 30.0f, 500.0f);
	touch_menu_down(&menu, 1, 2139.0f, 500.0f);
	touch_menu_up(&menu, 1, 2139.0f, 500.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.clicks == 2);
	/* above 1080 - 150 = 930 and from 20 down a swipe scrolls; the top
	zone is 20 high and the bottom one 150 */
	swipe(&menu, 1, 1170.0f, 929.0f, 1170.0f, 300.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.wheel_steps == -2);
	swipe(&menu, 1, 1170.0f, 930.0f, 1170.0f, 300.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.wheel_steps == 0);
	swipe(&menu, 1, 1170.0f, 19.0f, 1170.0f, 700.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.wheel_steps == 0);
	swipe(&menu, 1, 1170.0f, 20.0f, 1170.0f, 700.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.wheel_steps == 2);
}

/* a window size without insets: its far edge is not a zone */
static void test_without_insets_the_far_edges_are_no_zones(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;
	struct touch_menu_settings settings = { 36.0f, 120.0f, 2, 0.0f, 0.0f, 0.0f, 0.0f, 2340.0f, 1080.0f };

	touch_menu_init(&menu, &settings);
	touch_menu_down(&menu, 1, 2340.0f, 500.0f);
	touch_menu_up(&menu, 1, 2340.0f, 500.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.clicks == 1);
	CHECK(out.downs == 1);
	swipe(&menu, 1, 1170.0f, 1080.0f, 1170.0f, 300.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.wheel_steps == -2);
}

/* the phone rotates after the app started: the zones the host reads at a
down replace the old ones, for the next finger on */
static void test_zones_changed_between_fingers_apply_at_the_next_down(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;
	struct touch_menu_settings settings = { 36.0f, 120.0f, 2, 0.0f, 0.0f, 0.0f, 0.0f, 1080.0f, 2340.0f };

	touch_menu_init(&menu, &settings);
	touch_menu_down(&menu, 1, 10.0f, 500.0f);
	touch_menu_up(&menu, 1, 10.0f, 500.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.clicks == 1);
	menu.settings.edge_left = 90.0f;
	menu.settings.edge_right = 90.0f;
	menu.settings.width = 2340.0f;
	menu.settings.height = 1080.0f;
	touch_menu_down(&menu, 1, 10.0f, 500.0f);
	touch_menu_up(&menu, 1, 10.0f, 500.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.clicks == 0);
	CHECK(out.downs == 0);
	touch_menu_down(&menu, 1, 2300.0f, 500.0f);
	touch_menu_up(&menu, 1, 2300.0f, 500.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.clicks == 0);
	menu.settings.edge_left = 0.0f;
	menu.settings.edge_right = 0.0f;
	touch_menu_down(&menu, 1, 10.0f, 500.0f);
	touch_menu_up(&menu, 1, 10.0f, 500.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.clicks == 1);
}

/* a zone is judged at the down only: a finger already down keeps its kind
when the zones change under it */
static void test_zones_changed_under_a_finger_do_not_touch_it(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup_zones(&menu);
	touch_menu_down(&menu, 1, 1170.0f, 540.0f);
	menu.settings.edge_top = 600.0f;
	menu.settings.edge_left = 2000.0f;
	touch_menu_move(&menu, 1, 1170.0f, 100.0f);
	touch_menu_up(&menu, 1, 1170.0f, 100.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.wheel_steps == -2);
	CHECK(out.clicks == 0);
}

static void test_reset_and_cancel_drop_a_zone_finger(void)
{
	struct touch_menu menu;
	struct touch_menu_output out;

	setup_zones(&menu);
	touch_menu_down(&menu, 1, 10.0f, 500.0f);
	touch_menu_reset(&menu);
	touch_menu_down(&menu, 2, 1170.0f, 540.0f);
	touch_menu_up(&menu, 2, 1170.0f, 540.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.clicks == 1);
	/* a bottom finger that moved on is void; after a cancel the next
	finger taps */
	touch_menu_down(&menu, 1, 1170.0f, 1060.0f);
	touch_menu_move(&menu, 1, 1170.0f, 800.0f);
	touch_menu_cancel(&menu);
	touch_menu_down(&menu, 2, 1170.0f, 540.0f);
	touch_menu_up(&menu, 2, 1170.0f, 540.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.clicks == 1);
	CHECK(out.downs == 1);
	/* and after a reset */
	touch_menu_down(&menu, 1, 1170.0f, 1060.0f);
	touch_menu_move(&menu, 1, 1170.0f, 800.0f);
	touch_menu_reset(&menu);
	touch_menu_down(&menu, 2, 1170.0f, 540.0f);
	touch_menu_up(&menu, 2, 1170.0f, 540.0f);
	touch_menu_read(&menu, &out);
	CHECK(out.clicks == 1);
}

int main(void)
{
	test_init_stores_the_settings();
	if (failures)
		return 1;
	test_tap_clicks_where_the_finger_went_down();
	test_read_clears_the_tap();
	test_nothing_until_the_finger_lifts();
	test_a_tap_may_move_the_whole_slop();
	test_two_taps_between_reads();
	test_drag_down_scrolls_back_without_clicking();
	test_drag_up_scrolls_forward();
	test_horizontal_drag_uses_the_x_axis();
	test_equal_dx_dy_scrolls_vertically();
	test_the_axis_stays_after_the_start();
	test_a_fast_drag_counts_from_the_slop();
	test_steps_come_out_gradually();
	test_forward_steps_come_out_gradually();
	test_part_of_a_step_is_kept();
	test_lift_far_away_is_no_tap();
	test_a_scroll_keeps_the_last_tap_point_without_moving();
	test_a_new_finger_stops_pending_steps();
	test_second_finger_is_ignored();
	test_lift_of_another_finger_is_ignored();
	test_move_and_lift_without_down_do_nothing();
	test_cancel_drops_the_gesture();
	test_reset_clears_everything();
	test_reset_during_a_touch_ignores_its_lift();
	test_a_step_is_exactly_the_step_setting();
	test_horizontal_drag_right_scrolls_back();
	test_a_new_finger_starts_a_fresh_step();
	test_a_lifted_finger_cannot_move_again();
	test_reset_keeps_the_settings();
	test_cancel_keeps_a_finished_tap();
	test_one_step_a_read();
	test_a_drag_reports_where_the_finger_went_down();
	test_a_tap_reports_its_down();
	test_two_downs_between_reads_count_twice();
	test_a_second_finger_is_no_down();
	test_reset_and_cancel_drop_the_unread_downs();
	test_a_tap_in_the_left_zone_is_ignored();
	test_the_left_zone_ends_at_the_inset();
	test_the_right_zone_begins_at_width_minus_the_inset();
	test_an_edge_swipe_does_not_scroll_or_click();
	test_an_ignored_finger_does_not_stick();
	test_an_edge_finger_does_not_block_a_second_finger();
	test_an_edge_finger_leaves_another_fingers_scroll_alone();
	test_an_edge_finger_keeps_the_pending_steps();
	test_a_tap_in_the_bottom_zone_clicks();
	test_a_swipe_up_from_the_bottom_zone_does_nothing();
	test_a_swipe_down_from_the_top_zone_does_nothing();
	test_a_no_scroll_finger_stays_void_after_returning();
	test_a_no_scroll_finger_taps_within_the_slop();
	test_just_outside_the_zones_scrolls_as_usual();
	test_the_bottom_and_top_zones_end_at_their_insets();
	test_a_scroll_after_a_no_scroll_finger_still_scrolls();
	test_a_corner_touch_is_ignored();
	test_without_zones_the_edges_work();
	test_without_a_size_only_the_left_and_top_zones_exist();
	test_each_side_has_its_own_inset();
	test_without_insets_the_far_edges_are_no_zones();
	test_zones_changed_between_fingers_apply_at_the_next_down();
	test_zones_changed_under_a_finger_do_not_touch_it();
	test_reset_and_cancel_drop_a_zone_finger();
	if (failures)
		printf("%d failures\n", failures);
	else
		printf("all tests passed\n");
	return failures != 0;
}
