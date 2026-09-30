/*
PORT_CONFIG.H

The native ports' settings, read from config.toml (port_config.c): next to
the executable on the desktop, in the data folder (the one holding maps/)
on Android. A missing file is written with the defaults. Each setting can
also be set for one run with its HALO_* environment variable, which wins
over the file (the tools and the Android app pass settings that way).

Settings are named "section.key", as in the file: "display.vsync".
*/

#ifndef PORT_CONFIG_H
#define PORT_CONFIG_H

int config_boolean(const char *name);
long config_integer(const char *name);
double config_real(const char *name);
/* never NULL; "" when unset */
const char *config_string(const char *name);
/* sets a boolean setting, and writes it into config.toml (only its line
changes); 1 on success */
int config_write_boolean(const char *name, int value);
/* the same for the other types (a real is written with two decimals) */
int config_write_integer(const char *name, long value);
int config_write_real(const char *name, double value);
int config_write_string(const char *name, const char *value);
/* a number setting for now only, not written */
void config_set_real(const char *name, double value);
/* the setting's comment, as config.toml has it (lines apart); "" if none */
const char *config_comment(const char *name);

/* a map's console command ("map_name levels\\c10\\c10") into command: a
campaign level's name, a multiplayer map's or a scenario path; 0 if none */
int halo_map_command(const char *map, char *command, unsigned long size);
/* a console command the game runs on its next update (source/main/console.c,
through halo_timed_command_next) */
void halo_queue_command(const char *command);

#endif
