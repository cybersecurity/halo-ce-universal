/*
HALO_EXIT_GAME.H

Whether the main menu offers EXIT GAME (source/interface/ui_widget.c): only
in a desktop application. A build says its game is one with
HALO_DESKTOP_APPLICATION on the game's units, as Windows
(tools/windows_build.py) and Linux (tools/linux_build.py) do. Every other
build leaves it out and has no EXIT GAME: Android (tools/android_build.py),
whose game units define __linux__ too, and any other port until its build
adds the define, as a port to a desktop system (macOS) would and a phone's,
handheld's or console's would not. Nothing else needs to change.
*/

#ifndef HALO_EXIT_GAME_H
#define HALO_EXIT_GAME_H

#ifdef HALO_DESKTOP_APPLICATION
#define halo_exit_game_supported() 1
#else
#define halo_exit_game_supported() 0
#endif

#endif
