/*
TOUCH_CONTROLS.H

On-screen touch controls for the Android host library (libmain.so).

Where it lives: the host, not the guest. The game (guest) never sees touch
events; the host turns them into a virtual SDL gamepad, which the guest reads
through the same host_sdl_get_gamepads / host_sdl_gamepad_axis /
host_sdl_gamepad_button imports it uses for a real controller.

  1. data       TC_Widget / TC_LookZone, one SDL_FingerID per widget
  2. events     TC_HandleEvent() reads SDL_EVENT_FINGER_*
  3. injection  TC_Update() builds a TC_State (the buttons and the left stick)
                and gives it to a TC_Backend, the virtual gamepad; the view
                goes another way, TC_TakeLookMotion(): the swipe on the right
                half of the screen becomes mouse motion for the guest
  4. drawing    TC_Render() (touch_render_gles.c) draws with OpenGL ES 3, because
                the game owns the window's GL context (an SDL_Renderer cannot
                share it)
  5. editing    holding the "E" button for a second opens the layout editor:
                drag a control to move it, "+" and "-" resize all of them, "O"
                changes the opacity, "S-" and "S+" the swipe sensitivity, "R" restores the defaults, "OK" leaves.
                The layout is kept in touch_layout.txt in the app's data
                folder (next to config.toml).

Axes run from -1 to 1 with SDL's convention: +Y is down, forward is -1.
All coordinates are pixels of the window (SDL_GetWindowSizeInPixels).
*/

#ifndef TOUCH_CONTROLS_H
#define TOUCH_CONTROLS_H

#include <SDL3/SDL.h>

#define TC_MAX_WIDGETS 24

typedef enum TC_Action
{
	TC_ACT_NONE = -1,
	TC_ACT_FIRE = 0,
	TC_ACT_GRENADE,
	TC_ACT_JUMP,
	TC_ACT_ACTION_RELOAD,
	TC_ACT_MELEE,
	TC_ACT_SWITCH_WEAPON,
	TC_ACT_CROUCH,
	TC_ACT_FLASHLIGHT,
	TC_ACT_ZOOM,
	TC_ACT_PAUSE,
	TC_ACT_BACK,			/* the scoreboard in multiplayer */
	TC_ACT_COUNT
} TC_Action;

#define TC_BIT(a) (1u << (unsigned)(a))

typedef struct TC_State
{
	float move_x, move_y;	/* the virtual left stick */
	Uint32 buttons;			/* TC_BIT(TC_Action) */
} TC_State;

typedef enum TC_WidgetType
{
	TC_WIDGET_BUTTON,
	TC_WIDGET_STICK,
	TC_WIDGET_UI			/* the editor's own buttons */
} TC_WidgetType;

typedef enum TC_UiId
{
	TC_UI_NONE = 0,
	TC_UI_EDIT,				/* hold to open the editor */
	TC_UI_OK,
	TC_UI_RESET,
	TC_UI_SMALLER,
	TC_UI_BIGGER,
	TC_UI_OPACITY,
	TC_UI_LOOK_LESS,
	TC_UI_LOOK_MORE
} TC_UiId;

#define TC_STICK_ID 100		/* the id a widget is saved under (buttons use their action) */

typedef struct TC_Widget
{
	TC_WidgetType type;
	TC_Action action;		/* buttons */
	TC_UiId ui;				/* editor buttons */
	int id;					/* saved layout key; -1: not saved */
	char label[4];

	float fx, fy;			/* resting place, as a fraction of the window */
	float size;				/* radius, as a fraction of the window height */

	float cx, cy;			/* where it is now (pixels) */
	float home_x, home_y;	/* where it rests (pixels) */
	float radius;			/* pixels, with the size setting applied */
	SDL_FRect zone;			/* floating stick: where a touch captures it */

	bool active;			/* a finger holds it; finger 0 is a valid ID, */
	SDL_FingerID finger;	/* so this flag decides, not the ID */
	float grab_x, grab_y;	/* editor: finger offset from the center */
	float hold;				/* the "E" button: seconds held */
	bool latched;			/* the hold already did its job */

	float knob_x, knob_y;	/* sticks: knob offset in pixels */
	float axis_x, axis_y;	/* sticks: output after the dead zone */
} TC_Widget;

typedef struct TC_LookZone
{
	SDL_FRect rect;
	bool active;
	SDL_FingerID finger;
	float last_x, last_y;
	float accum_x, accum_y;	/* pixels moved since the last update */
} TC_LookZone;

typedef struct TC_Saved
{
	int id;
	float fx, fy;
} TC_Saved;

typedef struct TC_Backend
{
	const char *name;
	bool (*init)(void *userdata);
	void (*shutdown)(void *userdata);
	/* the touch controls turn on or off (a physical controller arrived or
	left): attach or detach the virtual device */
	void (*set_enabled)(void *userdata, bool enabled);
	void (*submit)(void *userdata, const TC_State *state);	/* each frame, when on */
} TC_Backend;

extern const TC_Backend TC_BackendVirtualGamepad;	/* touch_backend_vpad.c */

typedef struct TC_Context
{
	SDL_Window *window;
	SDL_Mutex *lock;	/* events, update and render run on different threads */
	const TC_Backend *backend;
	void *backend_userdata;
	bool backend_on;

	bool enabled;
	bool visible;
	bool edit;						/* the layout editor is open */
	bool floating_stick;			/* the stick appears where the thumb lands */
	bool auto_hide_with_gamepad;	/* a physical controller turns the HUD off */
	bool physical_gamepad_present;

	float opacity;				/* 0..1 */
	float scale;				/* size of all controls, 0.6..1.6 */
	float look_sensitivity;		/* swipe to view turn, 0.5..4 (see TC_TakeLookMotion) */
	float stick_deadzone;

	int view_w, view_h;			/* window size in pixels */
	Uint64 last_update_ns;
	char layout_path[512];

	TC_Widget widgets[TC_MAX_WIDGETS];
	int widget_count;
	TC_LookZone look;
	TC_State state;

	TC_Saved saved[TC_MAX_WIDGETS];	/* positions read from touch_layout.txt */
	int saved_count;
} TC_Context;

bool TC_Init(TC_Context *c, SDL_Window *window, const TC_Backend *backend, void *backend_userdata);
void TC_Shutdown(TC_Context *c);

/* true when the HUD consumed the event */
bool TC_HandleEvent(TC_Context *c, const SDL_Event *event);

/* once the event queue is drained, before the game reads its input */
void TC_Update(TC_Context *c);

/* The swipe of the right side of the screen, as the mouse motion (in
pixels) the game turns the view with: it is direct, one finger movement
turns the view by a fixed angle, not a speed like a stick. false when
there is none. The host sends it to the guest as an SDL_EVENT_MOUSE_MOTION. */
bool TC_TakeLookMotion(TC_Context *c, float *dx, float *dy);

/* before the buffer swap, on the thread that owns the GL context */
void TC_Render(TC_Context *c);

/* lifts every finger (the app went to the background, the layout changed) */
void TC_ReleaseAll(TC_Context *c);

/* does the widget show in the current mode? (c->lock held) */
bool TC_WidgetShown(const TC_Context *c, const TC_Widget *widget);

#endif
