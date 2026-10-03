/*
NETWORK_SERVER_MANAGER_INTERNAL.H

Private networking declarations shared by the server manager and its message
handler.  Keep these out of the widely included public manager header: the
January compiler is sensitive to declaration position even in unrelated code.
*/

#ifndef __NETWORK_SERVER_MANAGER_INTERNAL_H
#define __NETWORK_SERVER_MANAGER_INTERNAL_H
#pragma once

/* ---------- headers */

#include "cseries.h"
#include "networking/network_server_manager.h"

/* ---------- constants */

/* ---------- macros */

/* ---------- structures */

struct message_client_game_update;
struct network_connection;
struct network_game;
struct network_game_server;
struct network_game_server_client_machine;
struct network_machine;
struct network_player;

/* ---------- prototypes/NETWORK_SERVER_MANAGER.C */

word network_game_server_get_state(
	struct network_game_server *server,
	short *substate);
boolean network_game_server_game_is_open(
	struct network_game_server *server);
/* port: whether the machines are loading the game (its start sent) */
boolean network_game_server_game_is_loading(
	struct network_game_server *server);
/* joining a distributed game in progress (network_server_manager.c) */
boolean network_game_server_accepts_late_joins(
	struct network_game_server *server);
boolean network_game_server_client_machine_is_loaded(
	struct network_game_server *server,
	struct network_game_server_client_machine *machine);
void network_game_server_late_joiner_loaded(
	struct network_game_server *server,
	struct network_game_server_client_machine *machine);
/* the machine was heard from (its timeout, network_server_manager.c) */
void network_game_server_client_machine_heard(
	struct network_game_server *server,
	struct network_game_server_client_machine *machine);
/* the host's own machine (its local client's) */
boolean network_game_server_client_machine_is_local(
	struct network_game_server *server,
	struct network_game_server_client_machine *machine);
/* (network_server_message_handler.c) to one client machine, reliably */
boolean network_game_server_send_message_to_client_machine(
	struct network_game_server *server,
	struct network_game_server_client_machine *machine,
	void *message);
boolean network_game_server_send_game_settings_to_client_machine(
	struct network_game_server *server,
	struct network_game_server_client_machine *machine,
	void const *game,
	long game_size);
boolean network_game_server_game_is_valid(
	struct network_game_server *server);
boolean network_game_server_client_machine_is_joined_to_game(
	struct network_game_server *server,
	struct network_game_server_client_machine *machine);
boolean network_game_server_accept_client_machine_into_game(
	struct network_game_server *server,
	struct network_game_server_client_machine *client_machine);
boolean network_game_server_add_player_to_game(
	struct network_game_server *server,
	struct network_game_server_client_machine *client_machine,
	struct network_player *player);
boolean network_game_server_remove_player_from_game(
	struct network_game_server *server,
	struct network_game_server_client_machine *client_machine,
	struct network_player *player);
boolean network_game_server_adjust_machine_settings(
	struct network_game_server *server,
	struct network_game_server_client_machine *client_machine,
	struct network_machine *machine_settings);
void network_game_server_client_machine_game_loading_complete(
	struct network_game_server *server,
	struct network_game_server_client_machine *client_machine);
void network_game_server_client_machine_is_precached(
	struct network_game_server *server,
	struct network_game_server_client_machine *client_machine,
	char const *map_name);
void network_game_server_handle_client_update_packet(
	struct network_game_server *server,
	struct network_game_server_client_machine *client_machine,
	struct message_client_game_update *game_update);
boolean network_game_server_switch_machine_from_postgame_to_pregame(
	struct network_game_server *server,
	struct network_game_server_client_machine *client_machine);
void network_game_server_set_machine_hardware_id(
	struct network_game_server_client_machine *machine,
	char const *hardware_id);
void network_game_server_queue_player_for_addition(
	struct network_game_server *server,
	struct network_player *player);
struct network_machine *network_game_server_get_client_machine(
	struct network_game_server *server,
	struct network_game_server_client_machine *client_machine,
	long *machine_index);
struct network_connection *network_game_server_get_connection(
	struct network_game_server *server);
struct network_connection *network_game_server_get_client_connection(
	struct network_game_server_client_machine *client_machine);
struct network_connection *network_game_server_get_machine_connection(
	struct network_game_server *server,
	struct network_machine *machine);
struct network_game_server_client_machine *network_game_server_get_client_machine_at_index(
	struct network_game_server *server,
	long index);
struct network_game_server_client_machine *network_game_server_get_client_machine_at_address(
	struct network_game_server *server,
	unsigned long address);
struct network_game *network_game_server_get_game(
	struct network_game_server *server);
boolean network_game_server_remove_machine_from_game(
	struct network_game_server *server,
	struct network_machine *machine);
void network_game_server_update_countdown(
	struct network_game_server *server,
	short countdown_event);
/* the lobby takes changes (not once the game has started loading) */
boolean network_game_server_lobby_is_open(
	struct network_game_server *server);
/* a client machine's slower may add to the countdown (once each countdown) */
boolean network_game_server_client_machine_may_slow_countdown(
	struct network_game_server *server,
	struct network_game_server_client_machine *machine);
/* ---------- globals */

/* ---------- public code */

#endif // __NETWORK_SERVER_MANAGER_INTERNAL_H
