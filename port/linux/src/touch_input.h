/*
TOUCH_INPUT.H

The touchscreen (touch_input.c). Everything here runs on the game's main
thread. The finger events and touch_input_menu_read run under
sdl_platform.c's input lock; touch_input_gamepad, called from
XInputGetState, does not, and reads the same touch_menu state without it.
That is safe only because platform_pump_events and XInputGetState both run
on the game's main thread: a call from any other thread needs the lock.
*/

#ifndef TOUCH_INPUT_H
#define TOUCH_INPUT_H

#include "platform.h"
#include "sdl_platform.h"

#include <SDL3/SDL_events.h>

/**
 * @brief Feeds one finger event to the menus' pointer.
 * @param type SDL_EVENT_FINGER_DOWN, _MOTION, _UP or _CANCELED
 * @param finger the event; its x and y are 0..1 of the window
 */
void touch_input_event(unsigned int type, const SDL_TouchFingerEvent *finger);

/**
 * @brief The window lost the focus: every touch ends without effect.
 */
void touch_input_cancel(void);

/**
 * @brief Says whether the menus' pointer is used (a menu is up). Any
 * touch in progress is dropped, so that it does not act in the other mode.
 * @param active nonzero while a menu is up
 */
void touch_input_menu_set_active(int active);

/**
 * @brief What the fingers did in the menus since the last read.
 * @param pointer receives the taps and the wheel steps; marked as touch
 */
void touch_input_menu_read(struct platform_ui_pointer *pointer);

/**
 * @brief Adds the touch controls to controller 1's state. Outside the menus
 * a tap presses A for a few polls when a cinematic can be skipped.
 * @param pad the state XInputGetState is about to return
 */
void touch_input_gamepad(XINPUT_GAMEPAD *pad);

#endif
