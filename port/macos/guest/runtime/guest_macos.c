/*
GUEST_MACOS.C

What the desktop platform layer calls that the macOS port leaves out.

The port does not update itself (port/linux/src/updater.c is not part of
it): builds come from the local source tree, and a downloaded executable
would not match it.
*/

#include <SDL3/SDL.h>

void updater_start(void)
{
}

void updater_poll(SDL_Window *window)
{
	(void)window;
}
