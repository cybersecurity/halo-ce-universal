/*
NETWORK_CLIENT_MANAGER.H

header included in hcex build.
*/

#ifndef __NETWORK_CLIENT_MANAGER_H
#define __NETWORK_CLIENT_MANAGER_H
#pragma once

/* ---------- constants */

enum network_game_client_state
{
	_network_game_client_state_searching,
	_network_game_client_state_joining,
	_network_game_client_state_pregame,
	_network_game_client_state_ingame,
	_network_game_client_state_postgame,
	NUMBER_OF_NETWORK_GAME_CLIENT_STATES,
};

/* ---------- macros */

/* ---------- structures */

struct network_advertised_game;
struct network_connection;
struct network_game;
struct network_game_client;
struct network_join_parameters;
struct network_machine;
struct network_player;
struct transport_address;
struct message_server_game_advertise;
struct message_server_machine_accepted;
struct message_server_game_update;

/* ---------- prototypes/NETWORK_CLIENT_MANAGER.C */

void network_game_client_dispose(
	struct network_game_client *client);
void network_game_client_keep_alive(
	struct network_game_client *client);
short network_game_client_get_state(
	struct network_game_client *client,
	short *state_data);
boolean network_game_client_join_first_available_game(
	void);
#ifdef HALO_WEB
/* 0: still searching; 1: joining; -1: incompatible; -2: closed; -3: failed.
The optional address uses transport_address order, as transport_client_start
writes it (IPv4 octets from the most significant to least significant byte). */
short network_game_client_quick_join(unsigned long target_address);
boolean network_game_client_has_local_player(struct network_game_client *client, short controller_index);
#endif
boolean network_game_client_set_team(
	char team_index);
/* whether the advertised game's host has this machine's network version
(else the player is told, and it is not to be joined) */
boolean network_game_client_advertised_game_compatible(
	struct network_game_client *client,
	struct network_advertised_game const *game,
	boolean tell);
boolean network_game_client_initiate_join_game(
	struct network_game_client *client,
	struct network_advertised_game *game,
	struct network_join_parameters *join_parameters,
	struct transport_address *address);
boolean network_game_client_set_machine(
	struct network_game_client *client,
	struct network_machine *machine);
struct network_machine *network_game_client_get_machine(
	struct network_game_client *client);
short network_game_client_get_machine_index(
	struct network_game_client *client);
boolean network_game_client_advertised_game_is_valid(
	struct network_advertised_game *advertised_game);
struct network_advertised_game *network_game_client_get_available_games(
	struct network_game_client *client);
short network_game_client_get_error(
	struct network_game_client *client);
short network_game_client_get_seconds_to_game_start(
	struct network_game_client *client);
boolean network_game_client_write(
	struct network_connection *connection,
	word *message,
	word message_size,
	struct transport_address *address,
	boolean reliable);
boolean network_game_client_idle(
	struct network_game_client *client);
boolean network_game_client_add_player(
	struct network_game_client *client,
	short local_player_index);
boolean network_game_client_address_matches_server(
	struct network_game_client *client,
	struct transport_address *address);
void network_game_client_new_advertised_game(
	struct network_game_client *client,
	struct message_server_game_advertise *message_packet);
void network_game_client_ponged(
	struct network_game_client *client,
	struct transport_address *source_address,
	long timestamp);
void network_game_client_accepted_into_game(
	struct network_game_client *client,
	struct transport_address *source_address,
	struct message_server_machine_accepted *message_packet);
void network_game_client_rejected_by_game(
	struct network_game_client *client,
	struct transport_address *source_address,
	word rejection_code);
boolean network_game_client_game_settings_updated(
	struct network_game_client *client,
	struct network_game *message_packet);
void network_game_client_countdown_timer_update(
	struct network_game_client *client,
	short seconds_to_game_start);
boolean network_game_client_game_has_started(
	struct network_game_client *client);
void network_game_client_game_shutdown(
	struct network_game_client *client);
boolean network_game_client_handle_game_update(
	struct network_game_client *client,
	struct message_server_game_update *game_update);
boolean network_game_client_add_player_to_game(
	struct network_game_client *client,
	struct network_player *player);
boolean network_game_client_remove_player(
	struct network_game_client *client,
	struct network_player *player,
	long reason);
boolean network_game_client_update_local_player_data(
	struct network_game_client *client,
	struct network_player *player);
boolean network_game_client_request_remove_player(
	struct network_game_client *client,
	struct network_player *player);
boolean network_game_client_request_start_time_change(
	struct network_game_client *client,
	short request_type);
void network_game_client_switch_to_postgame(
	struct network_game_client *client);
boolean network_game_client_switch_to_pregame(
	struct network_game_client *client);
void network_game_client_reset(
	struct network_game_client *client,
	boolean teardown_connection);
struct network_game_client *network_game_client_create(
	void);
struct network_connection *network_game_client_get_connection(
	struct network_game_client *client);
void network_game_client_get_remote_server_address(
	struct network_game_client *client,
	struct transport_address *address);
struct network_game *network_game_client_get_game(
	struct network_game_client *client);
boolean network_game_client_server_has_started_game(
	struct network_game_client *client);
long network_game_client_get_next_update_number(
	struct network_game_client *client);
long unstrip_player_index(
	long player_index);

/* ---------- globals */

extern boolean allow_out_of_sync;
/* the host's game time when it told this client to start a game in progress,
16 bits of it (network_client_message_handler.c sets it) */
extern long network_game_client_late_join_time;
extern boolean network_game_client_dont_use_directly_in_use;
extern struct network_game_client network_game_client_dont_use_directly;

/* ---------- public code */

#endif // __NETWORK_CLIENT_MANAGER_H
