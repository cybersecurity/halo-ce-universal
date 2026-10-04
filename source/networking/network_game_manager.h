/*
NETWORK_GAME_MANAGER.H

header included in hcex build.
*/

#ifndef __NETWORK_GAME_MANAGER_H
#define __NETWORK_GAME_MANAGER_H
#pragma once

/* ---------- headers */

#include "game/game_engine.h"
#include "game/players.h"

/* ---------- constants */

/* ---------- macros */

/* ---------- structures */

struct network_game;
struct network_machine;
struct network_player;

struct network_machine
{
	wchar_t name[32];
	char machine_index;
	byte __padding41[3];
};

struct network_game_map
{
	long version;
	char name[0x80];
};

struct network_game_local_data
{
	boolean game_objects_loaded;
	byte __padding431[3];
};

struct network_game
{
	wchar_t name[16];
	struct network_game_map map;
	struct game_variant variant;
	byte __padding10C;
	char minimum_players;
	/* port: 128 does not fit a signed char */
	byte maximum_players;
	byte maximum_teams;
	short difficulty;
	short machine_count;
	/* port: the native builds' session limits, 128 machines and players
	(port/linux/include/halo_port_limits.h) */
	struct network_machine machines[HALO_PORT_MAXIMUM_NETWORK_MACHINES];
	short player_count;
	struct network_player players[HALO_PORT_MAXIMUM_NETWORK_PLAYERS];
	short __unknown426;
	unsigned long random_seed;
	long number_of_games_played;
	/* port: the gametype's PC options (game_engine.h) */
	struct game_variant_options variant_options;
	struct network_game_local_data local_data;
};

/* ---------- prototypes/EXAMPLE.C */

void network_game_invalidate_player(
	struct network_player *player);
void network_game_end_and_load_ui(
	struct network_game *game);
void network_game_assign_players_to_team(
	struct network_game *game,
	char const *prefix);
boolean network_player_is_valid(
	struct network_player *player);
boolean network_game_add_machine(
	struct network_game *game,
	struct network_machine *machine);
boolean network_game_update_machine(
	struct network_game *game,
	struct network_machine *machine);
void xbox_set_machine_name(
	char const *machine_name);
void network_game_generate_local_machine_name(
	wchar_t *machine_name);
boolean network_game_spawn_player(
	struct network_player *player);
boolean network_game_player_is_valid(
	struct network_player *player,
	struct network_game *game);
void network_game_reset_for_next_round(
	struct network_game *game,
	boolean unload_game_objects);
boolean network_game_add_player(
	struct network_game *game,
	struct network_player *player);
boolean network_game_has_free_player_slot(
	struct network_game *game);
void network_game_invalidate_machine(
	struct network_game *game,
	word machine_index);
void network_game_invalidate(
	struct network_game *game);
boolean network_game_update_player(
	struct network_game *game,
	struct network_player *player);
boolean network_game_remove_player(
	struct network_game *game,
	struct network_player *player);
boolean network_game_remove_machine(
	struct network_game *game,
	struct network_machine *machine);
boolean network_game_create_game_objects(
	struct network_game *game);

/* ---------- globals */

/* ---------- public code */

#endif // __NETWORK_GAME_MANAGER_H
