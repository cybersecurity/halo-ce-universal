/*
NETWORK_SERVER_MANAGER.H

header included in hcex build.
*/

#ifndef __NETWORK_SERVER_MANAGER_H
#define __NETWORK_SERVER_MANAGER_H
#pragma once

/* ---------- headers */

#include "cseries.h"

/* ---------- constants */

/* ---------- macros */

/* ---------- structures */

/* ---------- prototypes/NETWORK_SERVER_MANAGER.C */

struct network_game_server;
struct game_variant;

struct network_game_server *network_game_server_create(
	void);
void network_game_server_dispose(
	struct network_game_server *server);
boolean network_game_server_idle(
	struct network_game_server *server);
void network_game_server_open_game(
	struct network_game_server *server);
void network_game_server_switch_to_postgame(
	struct network_game_server *server);
boolean network_game_server_graceful_shutdown(
	struct network_game_server *server);
boolean network_game_server_reset_to_pregame(
	struct network_game_server *server);
void network_game_server_pause_countdown(
	struct network_game_server *server,
	boolean pause_countdown);
void network_game_generate_join_game_token(
	byte *join_token);
void network_game_server_kick_machine(
	long machine_index);
/* the host's ban command (console.c, hs.c) */
enum
{
	NETWORK_GAME_SERVER_NAME_TEXT_SIZE = 16,
};
boolean network_game_server_ban_player(
	char const *text);
short network_game_server_matching_player_names(
	char const *text,
	char (*names)[NETWORK_GAME_SERVER_NAME_TEXT_SIZE],
	short maximum_count);
unsigned long network_game_server_machine_address(
	long machine_index);
char const *network_game_server_machine_hardware_id(
	long machine_index);
void network_game_server_update_ticks(
	struct network_game_server *server,
	short tick_count);
void network_game_server_change_map_name(
	struct network_game_server *server,
	char const *map_name);
void network_game_server_change_game_variant(
	struct network_game_server *server,
	struct game_variant *variant);
#ifdef HALO_WEB
/* Validate the quick-play Slayer setup. Browser distributed System Link
also allows a solo start and remains open for in-progress joins. */
boolean network_game_server_enable_quick_play(struct network_game_server *server);
#endif

/* ---------- globals */

/* ---------- public code */

#endif // __NETWORK_SERVER_MANAGER_H
