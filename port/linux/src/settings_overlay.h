/*
SETTINGS_OVERLAY.H

The settings overlay (settings_overlay.c): a panel over the game, opened with
F10 (Command-comma on a Mac), in which the settings are changed while
playing. Positions are the window's pixels (as drawn, not SDL's points).
*/

#ifndef __HALO_SETTINGS_OVERLAY_H
#define __HALO_SETTINGS_OVERLAY_H

/* whether it is open; the game then gets no input */
int settings_overlay_active(void);
void settings_overlay_set_active(int active);
/* a key (an SDL scancode) pressed or let go while it is open */
void settings_overlay_key(int scancode, int down, int repeat);
/* the mouse while it is open: where it is, a button pressed or let go (SDL's
numbers), the wheel's notches (up: positive) */
void settings_overlay_mouse_motion(float x, float y);
void settings_overlay_mouse_button(int button, int down, float x, float y);
void settings_overlay_mouse_wheel(int steps);
/* the first controller while it is open (any thread): its digital buttons
(XINPUT_GAMEPAD_DPAD_*), and A and B */
void settings_overlay_gamepad(unsigned int buttons, int a, int b);
/* between frames, on the window's thread: the controller's presses, and
debug.settings_script's keys whose time has come */
void settings_overlay_update(void);
/* draws it into the window's framebuffer, width x height pixels (after the
game's picture, before the swap) */
void settings_overlay_draw(int width, int height);

#endif
