/*
TOUCH_CONTROLS.C

Layout, finger tracking, the layout editor and the update that turns widgets
into a TC_State. The drawing is in touch_render_gles.c, the virtual gamepad in
touch_backend_vpad.c.
*/

#include "host.h"
#include "touch_controls.h"

#include <stdio.h>

#define EDIT_HOLD_SECONDS 0.8f
#define SCALE_MIN 0.6f
#define SCALE_MAX 1.6f
#define LOOK_MIN 0.5f
#define LOOK_MAX 4.0f
#define LOOK_DEFAULT 1.5f

static const float opacity_steps[] = { 0.35f, 0.60f, 0.80f, 1.00f };

/* ---------- geometry */

static bool in_circle(float x, float y, float cx, float cy, float r)
{
	float dx = x - cx, dy = y - cy;

	return dx * dx + dy * dy <= r * r;
}

static bool in_rect(const SDL_FRect *rect, float x, float y)
{
	return x >= rect->x && x < rect->x + rect->w && y >= rect->y && y < rect->y + rect->h;
}

/* SDL gives a finger's position as 0..1 of the window */
static void finger_to_pixels(const TC_Context *c, const SDL_TouchFingerEvent *finger, float *x, float *y)
{
	*x = finger->x * (float)c->view_w;
	*y = finger->y * (float)c->view_h;
}

bool TC_WidgetShown(const TC_Context *c, const TC_Widget *widget)
{
	if (widget->type != TC_WIDGET_UI)
		return true;
	/* the "E" button is for playing, the others for editing */
	return widget->ui == TC_UI_EDIT ? !c->edit : c->edit;
}

/* ---------- the saved layout (touch_layout.txt) */

static void make_layout_path(TC_Context *c)
{
	const char *directory = NULL;

#ifdef SDL_PLATFORM_ANDROID
	/* the app's data folder, where config.toml and save/ are */
	directory = SDL_GetAndroidExternalStoragePath();
#endif
	SDL_snprintf(c->layout_path, sizeof(c->layout_path), "%s/touch_layout.txt", directory ? directory : ".");
}

static void load_layout(TC_Context *c)
{
	FILE *file = fopen(c->layout_path, "r");
	char line[128];
	int id;
	float a, b;

	c->saved_count = 0;
	if (!file)
		return;
	while (fgets(line, sizeof(line), file))
	{
		if (sscanf(line, "pos %d %f %f", &id, &a, &b) == 3)
		{
			if (c->saved_count < TC_MAX_WIDGETS)
			{
				c->saved[c->saved_count].id = id;
				c->saved[c->saved_count].fx = SDL_clamp(a, 0.0f, 1.0f);
				c->saved[c->saved_count].fy = SDL_clamp(b, 0.0f, 1.0f);
				c->saved_count++;
			}
		}
		else if (sscanf(line, "scale %f", &a) == 1)
		{
			c->scale = SDL_clamp(a, SCALE_MIN, SCALE_MAX);
		}
		else if (sscanf(line, "opacity %f", &a) == 1)
		{
			c->opacity = SDL_clamp(a, 0.2f, 1.0f);
		}
		else if (sscanf(line, "look %f", &a) == 1)
		{
			c->look_sensitivity = SDL_clamp(a, LOOK_MIN, LOOK_MAX);
		}
	}
	fclose(file);
}

static void save_layout(const TC_Context *c)
{
	FILE *file = fopen(c->layout_path, "w");
	int index;

	if (!file)
	{
		host_logf(HOST_LOG_WARN, "touch: cannot write %s", c->layout_path);
		return;
	}
	fprintf(file, "# touch controls layout; delete this file to restore the defaults\n");
	fprintf(file, "scale %.3f\n", c->scale);
	fprintf(file, "opacity %.3f\n", c->opacity);
	fprintf(file, "look %.3f\n", c->look_sensitivity);
	for (index = 0; index < c->widget_count; index++)
	{
		const TC_Widget *widget = &c->widgets[index];

		if (widget->id >= 0)
			fprintf(file, "pos %d %.4f %.4f\n", widget->id, widget->fx, widget->fy);
	}
	fclose(file);
}

/* ---------- layout */

static TC_Widget *add_widget(TC_Context *c, TC_WidgetType type)
{
	TC_Widget *widget;

	if (c->widget_count >= TC_MAX_WIDGETS)
		return NULL;
	widget = &c->widgets[c->widget_count++];
	SDL_zerop(widget);
	widget->type = type;
	widget->action = TC_ACT_NONE;
	widget->id = -1;
	return widget;
}

/* x, y and radius in pixels of the current window; kept as fractions */
static TC_Widget *add_round(TC_Context *c, TC_WidgetType type, float x, float y, float radius, const char *label)
{
	TC_Widget *widget = add_widget(c, type);

	if (!widget)
		return NULL;
	widget->fx = x / (float)c->view_w;
	widget->fy = y / (float)c->view_h;
	widget->size = radius / (float)c->view_h;
	SDL_strlcpy(widget->label, label, sizeof(widget->label));
	return widget;
}

static void add_button(TC_Context *c, TC_Action action, float x, float y, float radius, const char *label)
{
	TC_Widget *widget = add_round(c, TC_WIDGET_BUTTON, x, y, radius, label);

	if (widget)
	{
		widget->action = action;
		widget->id = (int)action;
	}
}

static void add_ui(TC_Context *c, TC_UiId ui, float x, float y, float radius, const char *label)
{
	TC_Widget *widget = add_round(c, TC_WIDGET_UI, x, y, radius, label);

	if (widget)
		widget->ui = ui;
}

static void release_all(TC_Context *c)
{
	int index;

	for (index = 0; index < c->widget_count; index++)
	{
		TC_Widget *widget = &c->widgets[index];

		widget->active = false;
		widget->hold = 0.0f;
		widget->latched = false;
		widget->knob_x = widget->knob_y = 0.0f;
		widget->axis_x = widget->axis_y = 0.0f;
		widget->cx = widget->home_x;
		widget->cy = widget->home_y;
	}
	c->look.active = false;
	c->look.accum_x = c->look.accum_y = 0.0f;
	SDL_zero(c->state);
}

/* fractions -> pixels, with the size setting and the screen edges applied */
static void layout_pixels(TC_Context *c)
{
	const float w = (float)c->view_w;
	const float h = (float)c->view_h;
	int index;

	for (index = 0; index < c->widget_count; index++)
	{
		TC_Widget *widget = &c->widgets[index];
		float radius = widget->size * h * (widget->type == TC_WIDGET_UI ? 1.0f : c->scale);
		float x = SDL_clamp(widget->fx * w, radius, SDL_max(radius, w - radius));
		float y = SDL_clamp(widget->fy * h, radius, SDL_max(radius, h - radius));

		widget->radius = radius;
		widget->home_x = widget->cx = x;
		widget->home_y = widget->cy = y;

		if (widget->type == TC_WIDGET_STICK)
		{
			/* the floating stick wakes up anywhere around its resting place */
			float half = 2.6f * radius;
			float x0 = SDL_max(0.0f, x - half), y0 = SDL_max(0.0f, y - half);
			float x1 = SDL_min(w, x + half), y1 = SDL_min(h, y + half);

			widget->zone = (SDL_FRect){ x0, y0, x1 - x0, y1 - y0 };
		}
	}
}

/* the default layout, then whatever touch_layout.txt moved */
static void build_layout(TC_Context *c)
{
	const float w = (float)c->view_w;
	const float h = (float)c->view_h;
	TC_Widget *stick;
	int index, saved;
	int row;
	static const struct { TC_UiId ui; const char *label; } row_buttons[] =
	{
		{ TC_UI_SMALLER, "-" }, { TC_UI_BIGGER, "+" }, { TC_UI_OPACITY, "O" },
		{ TC_UI_LOOK_LESS, "S-" }, { TC_UI_LOOK_MORE, "S+" },
		{ TC_UI_RESET, "R" }, { TC_UI_OK, "OK" },
	};

	release_all(c);
	c->widget_count = 0;

	stick = add_round(c, TC_WIDGET_STICK, 0.28f * h, h - 0.28f * h, 0.16f * h, "");
	if (stick)
		stick->id = TC_STICK_ID;

	add_button(c, TC_ACT_FIRE, w - 0.14f * h, h - 0.40f * h, 0.100f * h, "F");
	add_button(c, TC_ACT_JUMP, w - 0.14f * h, h - 0.17f * h, 0.080f * h, "A");
	add_button(c, TC_ACT_ACTION_RELOAD, w - 0.31f * h, h - 0.20f * h, 0.070f * h, "X");
	add_button(c, TC_ACT_MELEE, w - 0.33f * h, h - 0.37f * h, 0.060f * h, "B");
	add_button(c, TC_ACT_SWITCH_WEAPON, w - 0.33f * h, h - 0.56f * h, 0.060f * h, "Y");
	add_button(c, TC_ACT_GRENADE, w - 0.14f * h, h - 0.62f * h, 0.060f * h, "G");
	add_button(c, TC_ACT_CROUCH, 0.58f * h, h - 0.12f * h, 0.055f * h, "C");
	add_button(c, TC_ACT_ZOOM, w - 0.50f * h, h - 0.12f * h, 0.050f * h, "Z");
	add_button(c, TC_ACT_FLASHLIGHT, w * 0.5f + 0.12f * h, 0.07f * h, 0.045f * h, "L");
	add_button(c, TC_ACT_PAUSE, w * 0.5f, 0.07f * h, 0.045f * h, "II");
	add_button(c, TC_ACT_BACK, w - 0.07f * h, 0.07f * h, 0.045f * h, "BK");

	for (index = 0; index < c->widget_count; index++)
	{
		TC_Widget *widget = &c->widgets[index];

		for (saved = 0; saved < c->saved_count; saved++)
		{
			if (c->saved[saved].id == widget->id)
			{
				widget->fx = c->saved[saved].fx;
				widget->fy = c->saved[saved].fy;
			}
		}
	}

	/* the editor's buttons never move */
	add_ui(c, TC_UI_EDIT, w * 0.5f - 0.12f * h, 0.07f * h, 0.045f * h, "E");
	for (row = 0; row < (int)SDL_arraysize(row_buttons); row++)
		add_ui(c, row_buttons[row].ui, w * 0.5f + ((float)row - 3.0f) * 0.12f * h, 0.5f * h, 0.05f * h, row_buttons[row].label);

	/* anything that no control caught turns the view */
	c->look.rect = (SDL_FRect){ 0.0f, 0.0f, w, h };
	layout_pixels(c);
}

static void sync_layout(TC_Context *c)
{
	int w = 0, h = 0;

	if (!SDL_GetWindowSizeInPixels(c->window, &w, &h) || w <= 0 || h <= 0)
		return;
	if (w != c->view_w || h != c->view_h)
	{
		c->view_w = w;
		c->view_h = h;
		build_layout(c);
	}
}

/* ---------- physical controllers */

static void refresh_gamepads(TC_Context *c)
{
	int count = 0, index;
	bool physical = false;
	SDL_JoystickID *ids = SDL_GetGamepads(&count);

	if (ids)
	{
		for (index = 0; index < count; index++)
		{
			if (!SDL_IsJoystickVirtual(ids[index]))
				physical = true;
		}
		SDL_free(ids);
	}
	c->physical_gamepad_present = physical;
}

static bool is_on(const TC_Context *c)
{
	if (!c->enabled)
		return false;
	if (c->auto_hide_with_gamepad && c->physical_gamepad_present)
		return false;
	return true;
}

/* a virtual pad next to a real one would start a second (split screen)
player, so the backend attaches it only while the HUD is on */
static void apply_enabled(TC_Context *c)
{
	bool on = is_on(c);

	if (on == c->backend_on)
		return;
	c->backend_on = on;
	if (c->backend && c->backend->set_enabled)
		c->backend->set_enabled(c->backend_userdata, on);
	if (!on)
		release_all(c);
}

/* ---------- init */

bool TC_Init(TC_Context *c, SDL_Window *window, const TC_Backend *backend, void *backend_userdata)
{
	SDL_zerop(c);
	c->window = window;
	c->backend = backend;
	c->backend_userdata = backend_userdata;
	c->lock = SDL_CreateMutex();
	if (!window || !c->lock)
		return false;

	c->enabled = true;
	c->visible = true;
	c->floating_stick = true;
	c->auto_hide_with_gamepad = true;
	c->opacity = 0.80f;
	c->scale = 1.0f;
	c->look_sensitivity = LOOK_DEFAULT;
	c->stick_deadzone = 0.12f;
	make_layout_path(c);
	load_layout(c);

	if (backend && backend->init && !backend->init(backend_userdata))
	{
		host_logf(HOST_LOG_ERROR, "touch: backend %s failed", backend->name);
		SDL_DestroyMutex(c->lock);
		c->lock = NULL;
		return false;
	}

	SDL_LockMutex(c->lock);
	refresh_gamepads(c);
	sync_layout(c);
	apply_enabled(c);
	SDL_UnlockMutex(c->lock);
	c->last_update_ns = SDL_GetTicksNS();
	host_logf(HOST_LOG_INFO, "touch: layout file %s (%d saved positions)", c->layout_path, c->saved_count);
	return true;
}

void TC_Shutdown(TC_Context *c)
{
	if (!c->lock)
		return;
	if (c->backend && c->backend->shutdown)
		c->backend->shutdown(c->backend_userdata);
	SDL_DestroyMutex(c->lock);
	SDL_zerop(c);
}

void TC_ReleaseAll(TC_Context *c)
{
	SDL_LockMutex(c->lock);
	release_all(c);
	SDL_UnlockMutex(c->lock);
}

/* ---------- the editor */

static void ui_action(TC_Context *c, TC_UiId ui)
{
	size_t step;

	switch (ui)
	{
	case TC_UI_OK:
		c->edit = false;
		release_all(c);
		save_layout(c);
		break;
	case TC_UI_RESET:
		c->saved_count = 0;
		c->scale = 1.0f;
		c->opacity = 0.80f;
		c->look_sensitivity = LOOK_DEFAULT;
		build_layout(c);
		save_layout(c);
		break;
	case TC_UI_SMALLER:
	case TC_UI_BIGGER:
		c->scale = SDL_clamp(c->scale + (ui == TC_UI_BIGGER ? 0.1f : -0.1f), SCALE_MIN, SCALE_MAX);
		layout_pixels(c);
		save_layout(c);
		break;
	case TC_UI_OPACITY:
		/* the next step up, then back to the first */
		for (step = 0; step < SDL_arraysize(opacity_steps); step++)
		{
			if (opacity_steps[step] > c->opacity + 0.01f)
				break;
		}
		c->opacity = opacity_steps[step < SDL_arraysize(opacity_steps) ? step : 0];
		save_layout(c);
		break;
	case TC_UI_LOOK_LESS:
	case TC_UI_LOOK_MORE:
		c->look_sensitivity = SDL_clamp(c->look_sensitivity + (ui == TC_UI_LOOK_MORE ? 0.25f : -0.25f), LOOK_MIN, LOOK_MAX);
		save_layout(c);
		break;
	default:
		break;
	}
}

static bool edit_down(TC_Context *c, const SDL_TouchFingerEvent *finger)
{
	float x, y;
	int index;

	finger_to_pixels(c, finger, &x, &y);

	for (index = 0; index < c->widget_count; index++)
	{
		TC_Widget *widget = &c->widgets[index];

		if (widget->type != TC_WIDGET_UI || !TC_WidgetShown(c, widget))
			continue;
		if (in_circle(x, y, widget->cx, widget->cy, widget->radius * 1.15f))
		{
			ui_action(c, widget->ui);
			return true;
		}
	}
	for (index = 0; index < c->widget_count; index++)
	{
		TC_Widget *widget = &c->widgets[index];

		if (widget->type == TC_WIDGET_UI || widget->active)
			continue;
		if (in_circle(x, y, widget->cx, widget->cy, widget->radius * 1.15f))
		{
			widget->active = true;	/* here: being dragged */
			widget->finger = finger->fingerID;
			widget->grab_x = x - widget->cx;
			widget->grab_y = y - widget->cy;
			return true;
		}
	}
	return true;	/* nothing reaches the game while editing */
}

static bool edit_motion(TC_Context *c, const SDL_TouchFingerEvent *finger)
{
	float x, y;
	int index;

	finger_to_pixels(c, finger, &x, &y);
	for (index = 0; index < c->widget_count; index++)
	{
		TC_Widget *widget = &c->widgets[index];

		if (widget->active && widget->finger == finger->fingerID)
		{
			widget->fx = SDL_clamp((x - widget->grab_x) / (float)c->view_w, 0.0f, 1.0f);
			widget->fy = SDL_clamp((y - widget->grab_y) / (float)c->view_h, 0.0f, 1.0f);
			layout_pixels(c);
			return true;
		}
	}
	return true;
}

/* ---------- fingers while playing */

static void stick_move(TC_Widget *widget, float x, float y, float deadzone)
{
	float dx = x - widget->cx, dy = y - widget->cy;
	float length = SDL_sqrtf(dx * dx + dy * dy), magnitude;

	if (length > widget->radius && length > 0.0001f)
	{
		dx *= widget->radius / length;
		dy *= widget->radius / length;
		length = widget->radius;
	}
	widget->knob_x = dx;
	widget->knob_y = dy;

	magnitude = length / widget->radius;
	if (magnitude < deadzone || length < 0.0001f)
	{
		widget->axis_x = widget->axis_y = 0.0f;
	}
	else
	{
		/* radial dead zone, rescaled so the output starts from 0 */
		float scaled = (magnitude - deadzone) / (1.0f - deadzone);

		widget->axis_x = dx / length * scaled;
		widget->axis_y = dy / length * scaled;
	}
}

static bool finger_down(TC_Context *c, const SDL_TouchFingerEvent *finger)
{
	float x, y;
	int index;

	finger_to_pixels(c, finger, &x, &y);

	/* buttons, and the "E" that opens the editor when held */
	for (index = 0; index < c->widget_count; index++)
	{
		TC_Widget *widget = &c->widgets[index];

		if (widget->type == TC_WIDGET_STICK || widget->active || !TC_WidgetShown(c, widget))
			continue;
		if (in_circle(x, y, widget->cx, widget->cy, widget->radius * 1.15f))
		{
			widget->active = true;
			widget->finger = finger->fingerID;
			widget->hold = 0.0f;
			widget->latched = false;
			return true;
		}
	}

	for (index = 0; index < c->widget_count; index++)
	{
		TC_Widget *widget = &c->widgets[index];
		bool hit;

		if (widget->type != TC_WIDGET_STICK || widget->active)
			continue;
		hit = c->floating_stick ? in_rect(&widget->zone, x, y)
			: in_circle(x, y, widget->home_x, widget->home_y, widget->radius * 1.5f);
		if (!hit)
			continue;
		widget->active = true;
		widget->finger = finger->fingerID;
		widget->cx = c->floating_stick ? x : widget->home_x;
		widget->cy = c->floating_stick ? y : widget->home_y;
		widget->knob_x = widget->knob_y = 0.0f;
		widget->axis_x = widget->axis_y = 0.0f;
		if (!c->floating_stick)
			stick_move(widget, x, y, c->stick_deadzone);
		return true;
	}

	if (!c->look.active && in_rect(&c->look.rect, x, y))
	{
		c->look.active = true;
		c->look.finger = finger->fingerID;
		c->look.last_x = x;
		c->look.last_y = y;
		return true;
	}
	return false;
}

static bool finger_motion(TC_Context *c, const SDL_TouchFingerEvent *finger)
{
	float x, y;
	int index;

	finger_to_pixels(c, finger, &x, &y);

	for (index = 0; index < c->widget_count; index++)
	{
		TC_Widget *widget = &c->widgets[index];

		if (widget->active && widget->finger == finger->fingerID)
		{
			if (widget->type == TC_WIDGET_STICK)
				stick_move(widget, x, y, c->stick_deadzone);
			return true;
		}
	}
	if (c->look.active && c->look.finger == finger->fingerID)
	{
		c->look.accum_x += x - c->look.last_x;
		c->look.accum_y += y - c->look.last_y;
		c->look.last_x = x;
		c->look.last_y = y;
		return true;
	}
	return false;
}

static bool finger_up(TC_Context *c, const SDL_TouchFingerEvent *finger)
{
	int index;

	for (index = 0; index < c->widget_count; index++)
	{
		TC_Widget *widget = &c->widgets[index];

		if (widget->active && widget->finger == finger->fingerID)
		{
			widget->active = false;
			widget->hold = 0.0f;
			widget->latched = false;
			if (widget->type == TC_WIDGET_STICK)
			{
				widget->knob_x = widget->knob_y = 0.0f;
				widget->axis_x = widget->axis_y = 0.0f;
				widget->cx = widget->home_x;
				widget->cy = widget->home_y;
			}
			if (c->edit)
				save_layout(c);	/* the end of a drag */
			return true;
		}
	}
	if (c->look.active && c->look.finger == finger->fingerID)
	{
		c->look.active = false;
		return true;
	}
	return false;
}

bool TC_HandleEvent(TC_Context *c, const SDL_Event *event)
{
	bool consumed = false;

	if (!c->lock)
		return false;

	switch (event->type)
	{
	case SDL_EVENT_GAMEPAD_ADDED:
	case SDL_EVENT_GAMEPAD_REMOVED:
		SDL_LockMutex(c->lock);
		refresh_gamepads(c);
		SDL_UnlockMutex(c->lock);
		return false;

	case SDL_EVENT_WILL_ENTER_BACKGROUND:
	case SDL_EVENT_DID_ENTER_BACKGROUND:
		TC_ReleaseAll(c);
		return false;

	case SDL_EVENT_FINGER_DOWN:
	case SDL_EVENT_FINGER_MOTION:
	case SDL_EVENT_FINGER_UP:
	case SDL_EVENT_FINGER_CANCELED:
		break;

	default:
		return false;
	}

	/* touch SDL makes up from the mouse or a pen is not a finger */
	if (event->tfinger.touchID == SDL_MOUSE_TOUCHID || event->tfinger.touchID == SDL_PEN_TOUCHID)
		return false;

	SDL_LockMutex(c->lock);
	sync_layout(c);
	if (event->type == SDL_EVENT_FINGER_UP || event->type == SDL_EVENT_FINGER_CANCELED)
		consumed = finger_up(c, &event->tfinger);	/* always, so no finger stays held */
	else if (is_on(c) && c->edit)
		consumed = event->type == SDL_EVENT_FINGER_DOWN ? edit_down(c, &event->tfinger)
			: edit_motion(c, &event->tfinger);
	else if (is_on(c))
		consumed = event->type == SDL_EVENT_FINGER_DOWN ? finger_down(c, &event->tfinger)
			: finger_motion(c, &event->tfinger);
	SDL_UnlockMutex(c->lock);
	return consumed;
}

/* ---------- update */

void TC_Update(TC_Context *c)
{
	TC_State state;
	Uint64 now;
	float dt;
	int index;

	if (!c->lock)
		return;
	SDL_LockMutex(c->lock);

	now = SDL_GetTicksNS();
	dt = (float)(now - c->last_update_ns) / 1e9f;
	c->last_update_ns = now;

	sync_layout(c);
	apply_enabled(c);
	SDL_zero(state);

	if (c->backend_on && !c->edit)
	{
		for (index = 0; index < c->widget_count; index++)
		{
			TC_Widget *widget = &c->widgets[index];

			if (!widget->active)
				continue;
			if (widget->type == TC_WIDGET_BUTTON && widget->action != TC_ACT_NONE)
			{
				state.buttons |= TC_BIT(widget->action);
			}
			else if (widget->type == TC_WIDGET_STICK)
			{
				state.move_x = widget->axis_x;
				state.move_y = widget->axis_y;
			}
			else if (widget->type == TC_WIDGET_UI && widget->ui == TC_UI_EDIT && !widget->latched)
			{
				/* a deliberate hold, so a stray tap in a fight does not open the editor */
				widget->hold += SDL_min(dt, 0.1f);
				if (widget->hold >= EDIT_HOLD_SECONDS)
				{
					c->edit = true;
					release_all(c);	/* also lifts the finger from the game */
					break;
				}
			}
		}
	}
	c->state = state;

	/* in the editor this sends zeros, so nothing stays pressed */
	if (c->backend_on && c->backend && c->backend->submit)
		c->backend->submit(c->backend_userdata, &c->state);
	SDL_UnlockMutex(c->lock);
}

/* ---------- the view */

bool TC_TakeLookMotion(TC_Context *c, float *dx, float *dy)
{
	float scale;

	*dx = *dy = 0.0f;
	if (!c->lock)
		return false;
	SDL_LockMutex(c->lock);
	if (c->backend_on && !c->edit && c->view_h > 0)
	{
		/* pixels of a 720-line screen, whatever the phone's: a swipe the
		height of the screen then turns the view by the same angle on
		every phone */
		scale = c->look_sensitivity * 720.0f / (float)c->view_h;
		*dx = c->look.accum_x * scale;
		*dy = c->look.accum_y * scale;
	}
	c->look.accum_x = c->look.accum_y = 0.0f;
	SDL_UnlockMutex(c->lock);
	return *dx != 0.0f || *dy != 0.0f;
}
