/*
NETWORK_DISTRIBUTED.H

What the distributed netcode's modules share (port/linux/NETCODE.md):
network_distributed.c (the messages, the players, the game's state),
network_objects.c (the game's objects: who has which, where they are, what
units carry) and network_damage.c (damage: the host's, replayed on its
clients, and the clients' hits, reported to the host).
*/

#ifndef __NETWORK_DISTRIBUTED_H
#define __NETWORK_DISTRIBUTED_H
#pragma once

#include "bungie_net/common/message_header.h"
#include "networking/network_connection.h"

/* ---------- constants */

/* the messages, all of the game's "data" kind (network_distributed.c) */
enum
{
	/* a client's own players' units, every tick (unreliable) */
	_distributed_message_player_prediction = 1,
	/* every player's unit, every tick (unreliable) */
	_distributed_message_unit_states,
	/* the players' statistics (unreliable) */
	_distributed_message_player_statistics,
	/* what units carry (unreliable) */
	_distributed_message_inventories,
	/* objects created and deleted (reliable) */
	_distributed_message_object_changes,
	/* where objects are (unreliable) */
	_distributed_message_object_states,
	/* the game type's state (reliable) */
	_distributed_message_game_state,
	/* the host's objects all told (reliable, to one client) */
	_distributed_message_objects_synchronized,
	/* a client has loaded the game: it wants the host's objects (reliable) */
	_distributed_message_client_ready,
	/* damage the host dealt, for its clients' effects (unreliable: to the
	machines of the players it concerns, a kill to all) */
	_distributed_message_damage_events,
	/* a client's own players' hits (reliable) */
	_distributed_message_hit_reports,
	/* the vehicle a client's own player drives, every tick (unreliable) */
	_distributed_message_vehicle_prediction,
	/* what players picked up, for their clients to show (reliable, to
	each player's machine) */
	_distributed_message_pickups,
	/* a client's own players' input, every tick, each tick's buttons with
	the next ticks' (unreliable) */
	_distributed_message_player_inputs,
	/* every player's input as the host ran it, every tick, the same way
	(unreliable) */
	_distributed_message_relayed_actions,
	/* the unreliable messages of a tick to one machine, in one datagram */
	_distributed_message_batch,
	/* the host's text to every client, which shows it in red on its
	console (a player dropped for cheating: distributed_note_client_clock) */
	_distributed_message_notice,
	/* a client's Discord user, as its Discord told it (reliable, with its
	ready: distributed_client_send_identity) */
	_distributed_message_client_identity,
	/* every player's ping as the host measures it, every two seconds, for
	the scoreboard (unreliable) */
	_distributed_message_pings,
	/* a client's player's chat message, to the host (reliable: chat.c) */
	_distributed_message_chat_request,
	/* a chat message the host passes on, named, to every client or to those
	of the team (reliable) */
	_distributed_message_chat,

	NUMBER_OF_DISTRIBUTED_MESSAGES
};

/* where a message goes */
enum
{
	_distributed_to_clients,
	_distributed_to_clients_reliably,
	_distributed_to_host,
	_distributed_to_host_reliably,
};

/* how far part of an error is closed rather than jumped (world units): the
host's copy of a client's own player's unit or vehicle, and a client's copy
of the host's units and objects, which it moves half of the way there each
tick, drawn as it goes; further, it is put there, drawn gliding
(network_objects_reconcile). Later Halo engines do the same. */
#define HOST_BLEND_DISTANCE 0.25f
#define HOST_VEHICLE_BLEND_DISTANCE 0.5f
#define REMOTE_BLEND_DISTANCE 1.0f
#define REMOTE_VEHICLE_BLEND_DISTANCE 2.0f

enum
{
	/* the objects tracked by index: all of them */
	MAXIMUM_TRACKED_OBJECTS = HALO_PORT_MAXIMUM_OBJECTS_PER_MAP,
	MAXIMUM_TRACKED_PLAYERS = HALO_PORT_MAXIMUM_NETWORK_PLAYERS,
	/* a player index in a byte: none */
	NO_PLAYER = 0xFF,
};

/* ---------- structures */

struct distributed_message_header
{
	message_header header;
	byte type;
	byte count;
	long game_time;
};

/* a vector in 16-bit parts: a unit vector's of 1/32767, a velocity's of
1/1024 world units a tick, an angular velocity's of 1/4096 of a radian */
struct distributed_vector
{
	short i;
	short j;
	short k;
};

/* (what goes on the wire, as it is on every machine) */
typedef char distributed_message_header_size_assert[
	sizeof(struct distributed_message_header) == 8 ? 1 : -1];
typedef char distributed_message_header_game_time_offset_assert[
	offsetof(struct distributed_message_header, game_time) == 4 ? 1 : -1];
typedef char distributed_vector_size_assert[sizeof(struct distributed_vector) == 6 ? 1 : -1];

/* ---------- macros */

/* the entries of a type that fit one unreliable message, in a tick's batch
(its header, and the message's size and header but for its message header);
a longer message is split where a batch is full */
#define DATAGRAM_ENTRIES(type) \
	((short)((DATAGRAM_MAXIMUM_SIZE - 2 * sizeof(struct distributed_message_header) - sizeof(word) + \
		sizeof(message_header)) / sizeof(type)))
/* ... and one reliable message (its count a byte) */
#define RELIABLE_ENTRIES(type) \
	((short)MIN(255, (MAXIMUM_MESSAGE_SIZE - sizeof(struct distributed_message_header)) / sizeof(type)))

/* ---------- prototypes/NETWORK_DISTRIBUTED.C */

/* sends a message (its header filled in here) where destination says: the
unreliable ones gathered into one datagram a machine each tick */
void distributed_send(void *message, byte type, short count, word size, short destination);
void distributed_client_send_identity(void);
/* ... unreliably to one client (the host) */
void distributed_send_to_machine(long machine_index, void *message, byte type, short count, word size);
/* ... reliably to one client (the host) */
void distributed_send_to_machine_reliably(long machine_index, void *message, byte type, short count, word size);
/* the player at an absolute index, or NULL */
struct player_datum *distributed_player(short player_index);
/* a player's absolute index for a message, NO_PLAYER for none; and back */
byte distributed_player_to_byte(long player_index);
long distributed_player_from_byte(byte player_index);
/* whether the player is one of this machine's */
boolean distributed_player_is_local(long player_index);
/* the player's living unit, or NONE */
long distributed_living_unit(struct player_datum const *player);
/* whether the player is one of that client machine's (the host) */
boolean distributed_machine_has_player(long machine_index, short player_index);
void distributed_count_correction(void);
/* (the host) the client machines in the game, but for its own; their count */
short distributed_client_machines(long *machine_indices, short maximum);
/* (the host) how long a message takes that client and its answer back, in
ticks (and its jitter), as its players' input messages tell */
real distributed_machine_round_trip_ticks(long machine_index);
/* (a client) the host's latest tick it has had a message of, NONE for none */
long distributed_latest_host_time(void);
/* (a client) how long the host takes to have this machine's players and
tell it back, in ticks (0 before it is measured) */
real distributed_own_round_trip_ticks(void);
/* a player's ping (the round trip of its machine's messages to the host and
back, as the host measures it) in milliseconds: 0 for the host's own
players, NONE before it is known */
long distributed_player_ping(short player_index);
/* (the host, in its tick) the client machine a player is on, NONE for none
(the host's own players') */
long distributed_player_machine(short player_index);
/* (the host, in its tick) whether the client's players can see the player
(or the player is its own, or it has none alive to see with) */
boolean distributed_machine_sees_player(long machine_index, short player_index);
/* what a message says, checked: a number finite; a point's parts finite
and within bound of the origin; an object's index whole (its identifier
never 0, which any object at the index matches) */
boolean distributed_real_valid(real value);
boolean distributed_point_valid(real_point3d const *point, real bound);
boolean distributed_object_index_valid(long object_index);
/* ... an orientation's two axes (unpacked): TRUE when they are one long
and about square, then made exactly so */
boolean distributed_axes_make_valid(real_vector3d *forward, real_vector3d *up);
/* the vectors in 16 bits a part (struct distributed_vector) */
void distributed_vector_pack(real_vector3d const *vector, real scale, struct distributed_vector *result);
void distributed_vector_unpack(struct distributed_vector const *vector, real scale, real_vector3d *result);
void distributed_unit_vector_unpack(struct distributed_vector const *vector, real_vector3d *result);
#define DISTRIBUTED_UNIT_SCALE 32767.0f
#define DISTRIBUTED_VELOCITY_SCALE 1024.0f
#define DISTRIBUTED_ANGULAR_VELOCITY_SCALE 4096.0f

/* ---------- prototypes/NETWORK_OBJECTS.C */

void network_objects_new_game(void);
/* after each tick */
void network_objects_host_tick(void);
void network_objects_client_tick(void);
/* (the host) a client has loaded the game and asks for the host's objects:
again, having failed to make one of them */
void network_objects_client_asked(long machine_index, boolean again);
void network_objects_handle_changes(void const *entries, short count);
void network_objects_handle_synchronized(void);
void network_objects_handle_states(void const *entries, short count);
void network_objects_handle_inventories(void const *entries, short count);
void network_objects_handle_vehicle_prediction(long machine_index, void const *entries, short count);
/* (the host) the vehicle predictions come in since the last tick, taken */
void network_objects_apply_vehicle_predictions(void);
word network_objects_entry_size(byte type);
/* whether this client has the host's object at this index */
boolean network_objects_client_has(long object_index);
/* moves the object where the host has it, drawn gliding from where it was */
void network_objects_correct(long object_index, real_point3d const *position, real_vector3d const *forward,
	real_vector3d const *up, real_vector3d const *velocity, real_vector3d const *angular_velocity);
/* ... or, within blend_distance of it, half of the way there (drawn as it
moves); TRUE when it was put there */
boolean network_objects_reconcile(long object_index, real_point3d const *position, real_vector3d const *forward,
	real_vector3d const *up, real_vector3d const *velocity, real_vector3d const *angular_velocity,
	real blend_distance);
/* a unit in the vehicle's seat as the host has it (NONE: in none) */
void network_objects_set_seat(long unit_index, long vehicle_index, short seat_index);

/* ---------- prototypes/NETWORK_DAMAGE.C */

void network_damage_new_game(void);
void network_damage_host_tick(void);
void network_damage_client_tick(void);
void network_damage_handle_events(void const *entries, short count);
void network_damage_handle_reports(long machine_index, void const *entries, short count);
word network_damage_entry_size(byte type);

#endif // __NETWORK_DISTRIBUTED_H
