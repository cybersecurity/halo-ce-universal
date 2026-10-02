/*
GAME_SETTINGS.H

The settings of the menus' VIDEO, AUDIO and INPUT SETTINGS
(source/interface/ui_widget.c), each one of config.toml's
(port/linux/src/port_config.c), in the order of its sections: display (VIDEO
SETTINGS), audio, input. They are the game's, the same for every player
profile. Changing one changes it in the running game at once and writes it
into the file (port/linux/src/game_settings.c). Desktop builds only: Android
has no settings in its menus.
*/

#ifndef GAME_SETTINGS_H
#define GAME_SETTINGS_H

enum
{
	_game_setting_fullscreen,
	_game_setting_resolution,
	_game_setting_vsync,
	_game_setting_interpolation,
	_game_setting_direct_camera,
	_game_setting_window_scale,
	_game_setting_audio,
	_game_setting_master_volume,
	_game_setting_mouse_sensitivity,
	_game_setting_mouse_aim_assist,
	_game_setting_invert_mouse,
	NUMBER_OF_GAME_SETTINGS
};

/* whether the menus offer the settings; 0 on Android */
int game_settings_available(void);
/* the setting as it is in effect now (fullscreen as F11 last left it), as
the menu shows it: ON or OFF, a number, a percentage or a resolution
(1920 X 1080) */
void game_setting_text(int setting, char *text, int size);
/* whether the setting is on or off, rather than a number in a range or the
resolution */
int game_setting_is_switch(int setting);
/* turns a switch on or off, or steps a number or the resolution down
(direction < 0) or up, now and in config.toml; 0 if it could not change (at
the end of its range, or refused), and is as it was */
int game_setting_step(int setting, int direction);

#endif
