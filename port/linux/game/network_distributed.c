/*
NETWORK_DISTRIBUTED.C

The distributed netcode's own messages (port/linux/NETCODE.md): the game's
"data" message kind (message header type 2, which the Xbox game never sent),
beside its packets, and handled here (network_*_message_handler.c).

The host decides; its clients predict their own players and show the
rest as the host has it:

- The game's objects (units, vehicles, weapons, equipment) are the host's,
  at the same datum index on every machine (network_objects.c): the host
  says which it has, where they are and what units carry.
- Every tick, a client sends the host its own players' input, each tick's
  buttons again with the next three ticks' (a press is lost only with four
  datagrams in a row), and the host takes each tick's buttons once. The
  host sends every client the other players' input as its tick ran it, the
  same way, and the clients drive those players with it
  (player_queues_new.c).
- Every tick, a client sends the host where its own players' units are (it
  predicts them from its own input); the host takes that as they are,
  within a tolerance, as later Halo engines do, at its next tick.
- Every tick, the host sends every client every player's unit: which unit
  the player has, alive or not, the seat it rides, its shields and health
  (down, recharging, the damage they show), and where it is (dead: who
  killed it, which a client announces when its copy dies). A client binds,
  kills, seats and places its copies to match (it decides no deaths or
  spawns itself), and its own only when far off (a respawn, a teleport).
  Players far from a client's own, or out of their sight, are sent to it
  (their units and their input) less often, but at once when they come into
  sight, and their input every tick while their buttons change.
- Twice a second, and with every kill, the host sends the players'
  statistics (kills, deaths, ...) that changed, and once a second a few
  more round them all, which clients take as they are; a machine that has
  loaded, all of them.
- The host sends the game type's state (the scores, the flags, the balls
  and the hill) when it changes (looked at five times a second), once a
  second, and to a machine that has loaded, which clients take as it is
  (game_engine_write_network_state).
- Damage is the host's: a client reports its own players' hits, which the
  host checks and deals, and replays the damage the host deals for its
  effects (network_damage.c).

The unreliable messages of a tick to a machine go in as few datagrams (a
batch) as they fill, and each carries its tick (its header's game time):
one that arrives after a newer of its kind is dropped. Vectors travel in 16
bits a part, and a unit's state and a player's input only with the parts
that are not their defaults.

Players are named by their absolute index, which is the same on every
machine (their datum identifiers need not be).
*/

#include "cseries.h"
#include "cseries/errors.h"
#include "game/game.h"
#include "game/game_globals.h"
#include "game/players.h"
#include "game/player_queues_new.h"
#include "networking/network_game_globals.h"
#include "objects/objects.h"
#include "objects/damage.h"
#include "scenario/scenario.h"
#include "structures/structure_bsp_definitions.h"
#include "units/units.h"
#include "units/biped_definitions.h"
#include "units/bipeds.h"
#include "network_distributed.h"

#include <limits.h>
#include <stdio.h>
#include <time.h>
#include <math.h>

/* network_game_globals.c's and network_server_message_handler.c's */
boolean network_distributed_client_send(void *message, word size);
boolean network_distributed_client_send_reliably(void *message, word size);
boolean network_distributed_server_send_to_all_reliably(void *message, word size);
boolean network_distributed_server_send_to_machine(long machine_index, void *message, word size);
boolean network_distributed_server_send_to_machine_reliably(long machine_index, void *message, word size);
short network_distributed_server_machines(long *machine_indices, short maximum);
/* players.c's */
void network_player_attach_unit(long player_index, long unit_index);
void network_player_detach_unit(long player_index);
void network_player_show_pickup(long player_index, short kind, long definition_index, short count);
/* game_engine.c's */
long game_engine_write_network_state(byte *buffer, long size);
void game_engine_read_network_state(byte const *buffer, long size);
/* physics.c's (world units a tick, each tick) */
extern real global_gravity;
/* network_server_manager.c's, cseries_windows.c's, console.c's, p2p.c's */
void network_game_server_kick_machine(long machine_index);
unsigned long network_game_server_machine_address(long machine_index);
char const *network_game_server_machine_hardware_id(long machine_index);
void p2p_hardware_id_sanitize(char *destination, int size, const char *source);
void p2p_discord_sanitize(char *destination, int size, const char *source, int name);
void p2p_discord_identity(char *id, int id_size, char *name, int name_size);
unsigned long p2p_peer_endpoint_address(unsigned long virtual_address);
unsigned long system_milliseconds(void);
void console_warning(const char *format, ...);

/* the host: a client's game run faster than its clock (a speed hack, which
speeds up the machine's own clock: nothing on it can tell), found by its
ticks, which its messages are stamped with, going by faster than the
host's own clock's time, and ahead of the host's (a client's clock never
is: it starts at the host's, and only ever jumps forward to it, when it is
behind; a host that stalled has every client ahead, their ticks going by as
fast as time). Measured over each window this long (milliseconds)... */
#define CLIENT_CLOCK_WINDOW_MILLISECONDS 2000
/* ... faster than this many times as fast as time, and this many ticks
ahead of the host at its end: its players' predictions not taken (the
host's copies go as its own ticks have them) ... */
#define CLIENT_CLOCK_FAST_RATE 1.1f
#define CLIENT_CLOCK_AHEAD_TICKS 15
/* ... and so many windows in a row, dropped */
#define CLIENT_CLOCK_FAST_WINDOWS 5
/* the longest notice's text (_distributed_message_notice) */
#define MAXIMUM_NOTICE_LENGTH 160
/* a Discord user's id and name as kept, with their ends (p2p.h's
P2P_DISCORD_ID_SIZE and P2P_DISCORD_NAME_SIZE) */
#define DISCORD_ID_SIZE 24
#define DISCORD_NAME_SIZE 40
/* where the host logs the players it dropped for cheating, a line each */
#define CHEATERS_FILE "d:\\cheaters.txt"
/* ... and those it bans (by hand, and cheaters): their addresses refused */
#define BANS_FILE "d:\\bans.txt"

/* a client's Discord user, as told (_distributed_message_client_identity) */
struct distributed_client_identity
{
	char discord_id[DISCORD_ID_SIZE];
	char discord_name[DISCORD_NAME_SIZE];
};

enum
{
	STATISTICS_INTERVAL_TICKS = 15,
	/* the players' statistics sent unchanged once a second, round them all
	(the message is unreliable) */
	STATISTICS_REFRESH_TICKS = TICKS_PER_SECOND,
	STATISTICS_REFRESH_PLAYERS = 16,
	/* the game type's state looked at, and sent unchanged */
	GAME_STATE_INTERVAL_TICKS = 6,
	/* ... when it changes (reliably: no refresh), no more often than this
	(the king's and the ball's scores change every tick), but the game's end
	at once */
	GAME_STATE_MINIMUM_TICKS = 2 * GAME_STATE_INTERVAL_TICKS,
	MAXIMUM_GAME_STATE_SIZE = 0xF00,
	MAXIMUM_STATISTICS_PER_MESSAGE = 64,
	/* the players' pings sent, for the scoreboard */
	PING_INTERVAL_TICKS = 2 * TICKS_PER_SECOND,
	MAXIMUM_PINGS_PER_MESSAGE = 128,
	/* a ping not known */
	UNKNOWN_PING = 0xFFFF,
	MAXIMUM_PICKUPS_PER_TICK = 64,
	/* ticks a client's own player may ride where the host says it does not
	(or the other way round) before it is put where the host has it: its
	own prediction reaches the host and comes back in about a round trip */
	SEAT_DISAGREEMENT_TICKS = 15,
	/* the machines, and the host: a client's messages' sender */
	MAXIMUM_SENDERS = HALO_PORT_MAXIMUM_NETWORK_MACHINES + 1,
	HOST_SENDER = HALO_PORT_MAXIMUM_NETWORK_MACHINES,
	/* a round trip before one is measured, and the longest taken (ticks) */
	DEFAULT_ROUND_TRIP_TICKS = 6,
	MAXIMUM_ROUND_TRIP_TICKS = 60,
	/* ... and beyond it, the ticks a client's own player may ride otherwise
	than the host has it */
	SEAT_DISAGREEMENT_SLACK_TICKS = 6,
	/* a client: where its own players' units were, the last ticks (a power
	of two, more than the longest round trip) */
	OWN_POSITION_TICKS = 64,
	/* the host: the ticks after it took a client's player's position that it
	still tells the client which of its ticks it has them at */
	PREDICTION_ECHO_TICKS = 3,
	/* ... and the client's ticks it takes as run since the anchor beyond
	the host's (its messages delayed, then bunched) */
	PREDICTION_JITTER_TICKS = 6,
	/* ... and the most of a client's round trip it takes a client's player
	in the air to be ahead of its copy by (distributed_on_foot_ceiling) */
	PREDICTION_CEILING_LEAD_TICKS = 15,
	/* ... how long it measures predictions from one it took, the anchor,
	before it takes a newer as the anchor (the jitter and the blend granted
	once a second, not once a tick) */
	PREDICTION_ANCHOR_TICKS = TICKS_PER_SECOND,
	/* ... and how long it remembers how fast its own copy of a client's
	player went (an anchor's second, and the client's round trip: a client
	learns of an explosion that throws its player a round trip late) */
	PREDICTION_SPEED_TICKS = PREDICTION_ANCHOR_TICKS + MAXIMUM_ROUND_TRIP_TICKS,
};

/* struct distributed_unit_state flags */
enum
{
	/* the player has a unit that is alive */
	_distributed_unit_alive_bit = 0,
	/* ... and not riding (its position is its own) */
	_distributed_unit_placed_bit,
	/* (dead) how it died: killed by a teammate, or by an empty vehicle */
	_distributed_unit_friendly_fire_bit,
	_distributed_unit_killed_by_vehicle_bit,
	/* (alive) its shields: down, charging, overcharging */
	_distributed_unit_shield_depleted_bit,
	_distributed_unit_shield_charging_bit,
	_distributed_unit_shield_over_charging_bit,
	/* (alive) the host has the unit where the client's own prediction had
	it at the tick in predicted_time */
	_distributed_unit_predicted_bit,
};

/* struct distributed_unit_state unit flags */
enum
{
	/* (alive) camouflaged, and doubly so */
	_distributed_unit_camouflaged_bit = 0,
	_distributed_unit_super_camouflaged_bit,
};

/* a unit state's parts on the wire: which of those that are not always
there it has (distributed_unit_state_write); one it has not is its default */
enum
{
	/* (alive) the vehicle it rides and the seat */
	_distributed_unit_part_riding_bit = 0,
	/* (alive, placed) its up, when not straight up */
	_distributed_unit_part_up_bit,
	/* (alive) the damage its shields and body show, when any */
	_distributed_unit_part_damage_bit,
	/* (alive) how long its powerups have left, when any */
	_distributed_unit_part_powerups_bit,
	/* (alive) its camouflage, when any */
	_distributed_unit_part_camouflage_bit,
	/* (dead) who killed it, when anyone */
	_distributed_unit_part_killer_bit,
};

/* world units: how far a client's own player's unit may be from the host's
before the host takes it no longer, and before the client is put where the
host has it (no further than that: between the two they would disagree for
good, the host's player somewhere its own is not) */
#define HOST_ACCEPT_TOLERANCE 3.5f
#define REMOTE_CORRECTION_TOLERANCE 0.05f
#define LOCAL_CORRECTION_TOLERANCE 3.0f
/* how far from the origin a unit is (world units), and how fast a client's
own player's unit moves at most (world units a tick) */
#define UNIT_WORLD_BOUND 32768.0f
#define MAXIMUM_PREDICTED_SPEED 2.0f
/* ... on foot: this many times as fast as a player runs and jumps, or as
fast as the host's ticks threw its copy lately, whichever is more; and how
much further a tick than that it may be (world units a tick) */
#define PREDICTED_ON_FOOT_SPEED_SCALE 2.0f
#define PREDICTED_ON_FOOT_SPEED_MARGIN 0.05f
/* ... and how much higher than a jump takes it above where the host last
had it on the ground it may be (world units: steps climbed in the ticks a
prediction is late, where the host and the client have the ground) */
#define PREDICTED_RISE_TOLERANCE 1.0f

/* how far players are from a client's own (world units) before the host
sends them to it (their units and their input) every second tick, every
third, and every fourth; one no cluster of the client's players' can see
(the map's potentially visible set), or dead, every sixth. The set errs on
the side of seeing: a player comes into view in it before any line of sight
does, and is sent at once when they do, as a player whose buttons have
changed in the ticks their input carries is sent every tick. A player a client's player
aims near is sent at least every second tick, and every tick through a
scope (a sniper sees a far player as well as a near one, as Ares does).
(Cosines of the half angles.) */
#define NEAR_PLAYER_DISTANCE 25.0f
#define MIDDLE_PLAYER_DISTANCE 60.0f
#define FAR_PLAYER_DISTANCE 120.0f
#define AIMED_AT_COSINE 0.819f /* 35 degrees: on the screen */
#define SCOPED_AT_COSINE 0.940f /* 20 degrees: in a scope's view, and round it */
enum
{
	HIDDEN_PLAYER_PERIOD_TICKS = 6,
};

/* shields, health and the damage they show in 16 bits: 0 to 4 */
#define VITALITY_SCALE 16384.0f

/* a player's unit, as this machine has it (its bytes on the wire are fewer:
distributed_unit_state_write) */
struct distributed_unit_state
{
	byte player_index;
	byte flags;
	/* (dead) the player who killed it, NO_PLAYER for none */
	byte killing_player_index;
	byte unit_flags;
	/* the player's unit (the host's), NONE for none */
	long unit_index;
	/* the vehicle it rides and its seat, NONE for none */
	long vehicle_index;
	short seat_index;
	/* how camouflaged the unit is, of 255 */
	byte active_camouflage;
	byte pad;
	real_point3d position;
	struct distributed_vector velocity;
	struct distributed_vector forward;
	struct distributed_vector up;
	/* (_distributed_unit_predicted_bit) the client's tick, its low 16 bits */
	short predicted_time;
	/* (VITALITY_SCALE) */
	word body_vitality;
	word shield_vitality;
	/* what the shields' and the HUD's effects show */
	word current_body_damage;
	word recent_body_damage;
	word current_shield_damage;
	word recent_shield_damage;
	/* the player's powerups: how long each has left */
	short powerup_durations[NUMBER_OF_PLAYER_POWERUPS];
};

/* a client's player's input at one of its ticks, with the buttons of the
ticks before it */
struct distributed_player_input
{
	byte player_index;
	byte pad[3];
	/* the client's tick */
	long tick;
	/* the latest host tick the client has had a message of (the host
	measures the round trip by it) */
	long host_time;
	struct player_action action;
	unsigned short control_flags[DISTRIBUTED_INPUT_HISTORY];
};

typedef char distributed_player_input_size_assert[
	sizeof(struct distributed_player_input) == 12 + 0x20 + 2 * DISTRIBUTED_INPUT_HISTORY ? 1 : -1];
typedef char distributed_player_input_action_offset_assert[
	offsetof(struct distributed_player_input, action) == 12 ? 1 : -1];

/* a player's input as the host ran it, as this machine has it: the host's
update (the same for every player of a tick) goes once a message, and on
the wire the ticks' buttons before the latest only where they differ from
the tick after (distributed_relayed_action_write) */
struct distributed_relayed_action
{
	byte player_index;
	signed char desired_weapon_index;
	signed char desired_grenade_index;
	signed char desired_zoom_level;
	unsigned short control_flags[DISTRIBUTED_INPUT_HISTORY];
	/* of a turn */
	short yaw;
	short pitch;
	/* of 127, and 255 */
	signed char throttle_i;
	signed char throttle_j;
	byte primary_trigger;
};

/* a relayed action's parts on the wire: which of the ticks' buttons before
the latest it has (bit h - 1 for tick h), the others those of the tick
after */
typedef char distributed_relayed_action_parts_assert[DISTRIBUTED_INPUT_HISTORY - 1 <= 8 ? 1 : -1];

enum
{
	/* a unit state's bytes on the wire: its player, flags and parts, the
	most it has, and a relayed action's (past the update each message has
	first) */
	DISTRIBUTED_UNIT_STATE_MINIMUM_SIZE = 3,
	DISTRIBUTED_UNIT_STATE_MAXIMUM_SIZE = 3 + 4 + (4 + 2) + (12 + 3 * 6) + 2 + 2 * 2 + 4 * 2 +
		2 * NUMBER_OF_PLAYER_POWERUPS + 2 + 1,
	DISTRIBUTED_RELAYED_ACTION_MINIMUM_SIZE = 14,
	DISTRIBUTED_RELAYED_ACTION_MAXIMUM_SIZE = 14 + 2 * (DISTRIBUTED_INPUT_HISTORY - 1),
};

struct distributed_pickup
{
	byte player_index;
	byte kind;
	short count;
	long definition_index;
};

typedef char distributed_pickup_size_assert[sizeof(struct distributed_pickup) == 8 ? 1 : -1];

struct distributed_player_statistics
{
	short player_index;
	short pad;
	struct game_statistics statistics;
};

typedef char distributed_player_statistics_size_assert[
	sizeof(struct distributed_player_statistics) == 4 + sizeof(struct game_statistics) ? 1 : -1];

struct distributed_statistics_message
{
	struct distributed_message_header header;
	struct distributed_player_statistics players[MAXIMUM_STATISTICS_PER_MESSAGE];
};

/* a player's ping, in milliseconds (UNKNOWN_PING: not known) */
struct distributed_player_ping
{
	byte player_index;
	byte pad;
	word milliseconds;
};

typedef char distributed_player_ping_size_assert[sizeof(struct distributed_player_ping) == 4 ? 1 : -1];

struct distributed_pings_message
{
	struct distributed_message_header header;
	struct distributed_player_ping players[MAXIMUM_PINGS_PER_MESSAGE];
};

/* a batch's datagram: its header, then each message's size and its bytes
past its message header */
struct distributed_batch
{
	word size;
	byte data[DATAGRAM_MAXIMUM_SIZE];
};

/* what each message in a batch takes besides its entries: its size, and
its header but for its message header */
#define BATCH_MESSAGE_OVERHEAD \
	((word)(sizeof(word) + sizeof(struct distributed_message_header) - sizeof(message_header)))

/* messages of entries of a size of their own, to one machine (or the host),
each as long as the batch has room for (distributed_packer_add): nothing
else goes into that batch while it packs */
struct distributed_packer
{
	short sender;
	byte type;
	short count;
	/* the bytes every message has first (the relayed actions' update), and
	the bytes past the header so far, of room */
	word prefix_size;
	word size;
	word room;
	struct
	{
		struct distributed_message_header header;
		byte data[DATAGRAM_MAXIMUM_SIZE];
	} message;
};

/* ---------- globals */

static long distributed_last_sent_time = NONE;

/* how each player last died, by absolute index: the host's own, which it
sends its clients, and a client's copy of the host's */
static struct distributed_death
{
	boolean valid;
	/* the killer's absolute index, or NONE */
	short killing_player_index;
	boolean friendly_fire;
	boolean killed_by_vehicle;
} distributed_deaths[MAXIMUM_TRACKED_PLAYERS];
/* the host: a kill this tick, whose statistics the clients should have
with it */
static boolean distributed_statistics_due;
/* the host: what players on other machines picked up this tick */
static struct distributed_pickup distributed_pickups[MAXIMUM_PICKUPS_PER_TICK];
static short distributed_pickup_count;
/* a client: the ticks each of its own players has ridden other than as
the host has it */
static short distributed_seat_disagreements[MAXIMUM_TRACKED_PLAYERS];

/* the host: each client's player's latest prediction, taken at the next
tick, and the client's tick of the one taken last and when */
static struct
{
	boolean valid;
	long time;
	struct distributed_unit_state state;
	long taken_time;
	long taken_host_time;
} distributed_predictions[MAXIMUM_TRACKED_PLAYERS];
/* the host: the anchor, where it took each client's player to be (one
taken, a newer once a second: the next may be no further from it than the
player moves in the client's ticks since), at which of the client's ticks
and its own; and when it last took one, and where that left its unit */
static struct
{
	boolean valid;
	long unit_index;
	long time;
	long host_time;
	real_point3d position;
	long taken_host_time;
	real_point3d taken_host_position;
} distributed_accepted[MAXIMUM_TRACKED_PLAYERS];
/* ... what its own ticks did to its copy of each client's player's unit on
foot, beyond the client's word it took (an explosion's throw, a jump): how
fast they sent it, and how fast up, the most of this span of
PREDICTION_SPEED_TICKS and of the one before; the unit (NONE: none on
foot), the velocity it took (what its next tick started from) and whether
the unit was on the ground then; where it last had the unit on the ground
(or where it started: a new life, a ride's end, a teleporter) and the
highest it has had it since; and those speeds kept through a flight begun
while it had them (a throw's flight outlasting the span) */
static struct distributed_host_speed
{
	long time;
	real speeds[2];
	real rises[2];
	real flight_speed;
	real flight_rise;
	long unit_index;
	real_vector3d velocity;
	boolean grounded;
	real ground_height;
	real top_height;
	/* the last tick it had the unit on the ground (or where it started) */
	long ground_time;
} distributed_host_speeds[MAXIMUM_TRACKED_PLAYERS];
/* a client: where each of its own players' units was at its last ticks,
and how long the host takes to have them (ticks) */
static struct distributed_own_position
{
	long time;
	long unit_index;
	real_point3d position;
} distributed_own_positions[MAXIMUM_LOCAL_PLAYERS][OWN_POSITION_TICKS];
static real distributed_own_round_trip;
/* the host: the players each machine had when it last told the host that it
had loaded (a machine new at its index has none of the old one's state) */
static long distributed_machine_players[HALO_PORT_MAXIMUM_NETWORK_MACHINES][MAXIMUM_LOCAL_PLAYERS];
/* the host: what each player's unit was last sent as (a change goes to
every client at once) */
static struct
{
	byte flags;
	long unit_index;
	long vehicle_index;
	short seat_index;
} distributed_sent_units[MAXIMUM_TRACKED_PLAYERS];

/* the host: whether each client's players could see each player last tick
(one who comes into sight is sent at once) */
static boolean distributed_seen[HALO_PORT_MAXIMUM_NETWORK_MACHINES][MAXIMUM_TRACKED_PLAYERS];
/* the host: where each client's players were when last alive (a client
whose players are dead watches from there) */
static struct distributed_viewer
{
	boolean valid;
	short cluster_index;
	real_point3d position;
	real_vector3d aim;
} distributed_viewers[HALO_PORT_MAXIMUM_NETWORK_MACHINES][MAXIMUM_LOCAL_PLAYERS];
/* the host, this tick: every player's unit and input, what each client is
sent of each player (decided before the damage, which goes where the
players it hurt do), and the client each player is on (NONE for none: the
host's own) */
enum
{
	_distributed_send_state_bit = 0,
	_distributed_send_action_bit,
	/* the client's players can see the player (whether its state goes this
	tick or not): what hurts the player it is told of */
	_distributed_send_sees_bit,
};
static struct distributed_unit_state distributed_host_states[MAXIMUM_TRACKED_PLAYERS];
static struct distributed_relayed_action distributed_host_actions[MAXIMUM_TRACKED_PLAYERS];
static long distributed_host_update_number;
static byte distributed_sends[HALO_PORT_MAXIMUM_NETWORK_MACHINES][MAXIMUM_TRACKED_PLAYERS];
static long distributed_player_machines[MAXIMUM_TRACKED_PLAYERS];
/* the host: each player's statistics as last sent (only a change is sent,
and once a second a few players' whatever they are, round them all) */
static unsigned long distributed_sent_statistics[MAXIMUM_TRACKED_PLAYERS];
static short distributed_statistics_cursor;
/* the host: the game type's state as last sent (its checksum and its
leading postgame state), and when */
static unsigned long distributed_game_state_checksum;
static long distributed_game_state_postgame;
static long distributed_game_state_time;

/* the latest tick of each kind of unreliable message had from each sender
(a machine, or the host), NONE for none */
static long distributed_received_times[MAXIMUM_SENDERS][NUMBER_OF_DISTRIBUTED_MESSAGES];
/* the host: each client machine's clock, measured (CLIENT_CLOCK_WINDOW_MILLISECONDS):
its latest tick, and its tick and the host's time as the window began
(NONE: none begun); how many windows in a row it went fast, and whether
the last did */
/* the host: each client machine's Discord user, as it told it, the text
kept only of what is allowed (p2p_discord_sanitize) */
static struct distributed_client_identity distributed_client_identities[HALO_PORT_MAXIMUM_NETWORK_MACHINES];
/* a client: its Discord user as last sent this game, and whether it was */
static struct distributed_client_identity distributed_sent_identity;
static boolean distributed_identity_sent;
static struct distributed_client_clock
{
	long latest_tick;
	long window_tick;
	unsigned long window_time;
	short fast_windows;
	boolean fast;
} distributed_client_clocks[HALO_PORT_MAXIMUM_NETWORK_MACHINES];
/* a client: the host's latest tick it has had a message of */
static long distributed_host_time = NONE;
/* the host: each client's round trip, in ticks, and its jitter */
static struct
{
	boolean valid;
	real average;
	real deviation;
} distributed_round_trips[HALO_PORT_MAXIMUM_NETWORK_MACHINES];
/* a client: each player's ping as the host last told it (UNKNOWN_PING: not
told) */
static word distributed_player_pings[MAXIMUM_TRACKED_PLAYERS];

/* the host: its clients' machines, found once a tick */
static struct
{
	boolean in_tick;
	boolean valid;
	short count;
	long indices[HALO_PORT_MAXIMUM_NETWORK_MACHINES];
} distributed_machines;

/* the unreliable messages of this tick, a batch for each machine and one
for the host */
static struct distributed_batch distributed_batches[MAXIMUM_SENDERS];
/* (the largest messages, packed a machine at a time) */
static struct distributed_packer distributed_packer;

/* for the automated tests' reports (network_test.c) */
static struct
{
	long sent;
	long received;
	long corrections;
} distributed_statistics;

/* ---------- shared (network_distributed.h) */

void network_distributed_statistics(
	long *sent,
	long *received,
	long *corrections)
{
	*sent = distributed_statistics.sent;
	*received = distributed_statistics.received;
	*corrections = distributed_statistics.corrections;
}

void distributed_count_correction(
	void)
{
	distributed_statistics.corrections++;
}

boolean distributed_real_valid(
	real value)
{
	/* (NaN and the infinities less themselves are not 0) */
	return value - value == 0.0f;
}

boolean distributed_point_valid(
	real_point3d const *point,
	real bound)
{
	return fabsf(point->x) <= bound && fabsf(point->y) <= bound && fabsf(point->z) <= bound;
}

boolean distributed_object_index_valid(
	long object_index)
{
	return object_index != NONE && DATUM_INDEX_TO_IDENTIFIER(object_index) != 0;
}

boolean distributed_axes_make_valid(
	real_vector3d *forward,
	real_vector3d *up)
{
	real forward_length = (real)sqrt(forward->i * forward->i + forward->j * forward->j + forward->k * forward->k);
	real up_length = (real)sqrt(up->i * up->i + up->j * up->j + up->k * up->k);
	real dot;
	real length;

	if (!(forward_length > 0.5f && forward_length < 1.5f && up_length > 0.5f && up_length < 1.5f))
		return FALSE;
	forward->i /= forward_length;
	forward->j /= forward_length;
	forward->k /= forward_length;
	dot = (forward->i * up->i + forward->j * up->j + forward->k * up->k) / up_length;
	if (!(fabsf(dot) < 0.1f))
		return FALSE;
	/* (up square to forward) */
	up->i -= dot * up_length * forward->i;
	up->j -= dot * up_length * forward->j;
	up->k -= dot * up_length * forward->k;
	length = (real)sqrt(up->i * up->i + up->j * up->j + up->k * up->k);
	up->i /= length;
	up->j /= length;
	up->k /= length;
	return TRUE;
}

void distributed_vector_pack(
	real_vector3d const *vector,
	real scale,
	struct distributed_vector *result)
{
	real parts[3];
	short index;

	parts[0] = vector->i;
	parts[1] = vector->j;
	parts[2] = vector->k;
	for (index = 0; index < 3; index++)
	{
		real value = parts[index] * scale;

		value = value > 32767.0f ? 32767.0f : value < -32767.0f ? -32767.0f : value;
		parts[index] = (real)floor(value + 0.5f);
	}
	result->i = (short)parts[0];
	result->j = (short)parts[1];
	result->k = (short)parts[2];
}

void distributed_vector_unpack(
	struct distributed_vector const *vector,
	real scale,
	real_vector3d *result)
{
	result->i = (real)vector->i / scale;
	result->j = (real)vector->j / scale;
	result->k = (real)vector->k / scale;
}

void distributed_unit_vector_unpack(
	struct distributed_vector const *vector,
	real_vector3d *result)
{
	real length;

	distributed_vector_unpack(vector, DISTRIBUTED_UNIT_SCALE, result);
	length = (real)sqrt(result->i * result->i + result->j * result->j + result->k * result->k);
	if (length > 0.0f)
	{
		result->i /= length;
		result->j /= length;
		result->k /= length;
	}
}

static word distributed_vitality_pack(
	real value)
{
	value *= VITALITY_SCALE;
	value = value > 65535.0f ? 65535.0f : value < 0.0f ? 0.0f : value;
	return (word)(long)floor(value + 0.5f);
}

static real distributed_vitality_unpack(
	word value)
{
	return (real)value / VITALITY_SCALE;
}

/* an angle as a 16-bit fraction of a turn, and back (yaw from 0 to 2 pi,
pitch from -pi to pi) */
static short distributed_angle_pack(
	real angle)
{
	real turns = angle / (2.0f * _pi);

	turns -= (real)floor(turns);
	return (short)(word)((long)floor(turns * 65536.0f + 0.5f) & 0xFFFF);
}

static real distributed_angle_unpack(
	short value,
	boolean signed_angle)
{
	real angle = (real)(word)value * (2.0f * _pi) / 65536.0f;

	if (signed_angle && angle >= _pi)
		angle -= 2.0f * _pi;
	return angle;
}

static void distributed_batch_flush(
	short sender)
{
	struct distributed_batch *batch = &distributed_batches[sender];
	struct distributed_message_header *header = (struct distributed_message_header *)batch->data;

	if (!batch->size)
		return;
	header->type = _distributed_message_batch;
	header->count = 0;
	header->game_time = game_time_get();
	header->header = 0;
	build_message_header(&header->header, batch->size, 2, 0);
	if (sender == HOST_SENDER)
		network_distributed_client_send(batch->data, batch->size);
	else
		network_distributed_server_send_to_machine(sender, batch->data, batch->size);
	batch->size = 0;
}

/* the bytes a message's entries (past its header) have in the machine's
batch now; when fewer than minimum, the batch is sent first, and they are
an empty batch's */
static word distributed_batch_room(
	short sender,
	word minimum)
{
	struct distributed_batch *batch = &distributed_batches[sender];
	word used = batch->size ? batch->size : (word)sizeof(struct distributed_message_header);

	if (used + BATCH_MESSAGE_OVERHEAD + minimum > DATAGRAM_MAXIMUM_SIZE)
	{
		distributed_batch_flush(sender);
		used = sizeof(struct distributed_message_header);
	}
	return (word)(DATAGRAM_MAXIMUM_SIZE - used - BATCH_MESSAGE_OVERHEAD);
}

/* a message's header and entries into the machine's batch, which has room
for them */
static void distributed_batch_append(
	short sender,
	struct distributed_message_header const *header,
	byte const *entries,
	word entries_size)
{
	struct distributed_batch *batch = &distributed_batches[sender];
	word length = (word)(sizeof(*header) - sizeof(message_header) + entries_size);

	if (!batch->size)
		batch->size = sizeof(struct distributed_message_header);
	csmemcpy(batch->data + batch->size, &length, sizeof(word));
	csmemcpy(batch->data + batch->size + sizeof(word), (byte const *)header + sizeof(message_header),
		sizeof(*header) - sizeof(message_header));
	csmemcpy(batch->data + batch->size + BATCH_MESSAGE_OVERHEAD, entries, entries_size);
	batch->size += (word)(BATCH_MESSAGE_OVERHEAD + entries_size);
}

/* where a message's bytes past its message header go: a machine's batch,
sent at the tick's end (or sooner, full). A message of entries of
entry_size bytes each is split where a batch is full, the rest going in
the next; one not to be split (entry_size 0) goes whole, and one larger
than a datagram, which is a mistake, is dropped. */
static void distributed_batch_add(
	short sender,
	void const *message,
	word size,
	word entry_size)
{
	struct distributed_message_header header;
	byte const *entries = (byte const *)message + sizeof(header);
	word entries_size;
	short count;

	if (size < sizeof(header))
		return;
	csmemcpy(&header, message, sizeof(header));
	entries_size = (word)(size - sizeof(header));
	count = header.count;
	if (!entry_size || !count || entries_size != count * entry_size)
	{
		if (distributed_batch_room(sender, entries_size) < entries_size)
		{
			error(_error_silent, "distributed message of type #%d is #%d bytes, too large for a datagram; dropped",
				header.type, size);
			return;
		}
		distributed_batch_append(sender, &header, entries, entries_size);
		return;
	}
	while (count > 0)
	{
		short fit = (short)MIN(count, distributed_batch_room(sender, entry_size) / entry_size);

		if (fit <= 0)
		{
			error(_error_silent,
				"distributed message of type #%d has entries of #%d bytes, too large for a datagram; dropped",
				header.type, entry_size);
			return;
		}
		header.count = (byte)fit;
		distributed_batch_append(sender, &header, entries, (word)(fit * entry_size));
		entries += fit * entry_size;
		count -= fit;
	}
}

/* (the machines' datagrams in a turn that starts one further each tick:
none always last) */
static void distributed_batches_flush(
	void)
{
	short first = (short)((unsigned long)game_time_get() % MAXIMUM_SENDERS);
	short step;

	for (step = 0; step < MAXIMUM_SENDERS; step++)
		distributed_batch_flush((short)((first + step) % MAXIMUM_SENDERS));
}

static void distributed_fill_header(
	void *message,
	byte type,
	short count,
	word size)
{
	struct distributed_message_header *header = (struct distributed_message_header *)message;

	header->type = type;
	header->count = (byte)count;
	header->game_time = game_time_get();
	header->header = 0;
	build_message_header(&header->header, size, 2, 0);
	distributed_statistics.sent++;
}

/* the size of each of a message's entries, when they are alike, else 0 */
static word distributed_entry_size(
	short count,
	word size)
{
	word entries_size = (word)(size - sizeof(struct distributed_message_header));

	if (count <= 0 || size <= sizeof(struct distributed_message_header) || entries_size % count)
		return 0;
	return (word)(entries_size / count);
}

void distributed_send(
	void *message,
	byte type,
	short count,
	word size,
	short destination)
{
	distributed_fill_header(message, type, count, size);
	switch (destination)
	{
	case _distributed_to_clients:
	{
		long machine_indices[HALO_PORT_MAXIMUM_NETWORK_MACHINES];
		short machine_count = distributed_client_machines(machine_indices, HALO_PORT_MAXIMUM_NETWORK_MACHINES);
		short index;

		for (index = 0; index < machine_count; index++)
			distributed_batch_add((short)machine_indices[index], message, size, distributed_entry_size(count, size));
		break;
	}
	case _distributed_to_clients_reliably: network_distributed_server_send_to_all_reliably(message, size); break;
	case _distributed_to_host:
		distributed_batch_add(HOST_SENDER, message, size, distributed_entry_size(count, size));
		break;
	case _distributed_to_host_reliably: network_distributed_client_send_reliably(message, size); break;
	}
}

void distributed_send_to_machine(
	long machine_index,
	void *message,
	byte type,
	short count,
	word size)
{
	if (machine_index < 0 || machine_index >= HALO_PORT_MAXIMUM_NETWORK_MACHINES)
		return;
	distributed_fill_header(message, type, count, size);
	distributed_batch_add((short)machine_index, message, size, distributed_entry_size(count, size));
}

void distributed_send_to_machine_reliably(
	long machine_index,
	void *message,
	byte type,
	short count,
	word size)
{
	distributed_fill_header(message, type, count, size);
	network_distributed_server_send_to_machine_reliably(machine_index, message, size);
}

/* a packer's messages of the type, to the machine (or the host), each
beginning with the prefix */
static void distributed_packer_begin(
	struct distributed_packer *packer,
	short sender,
	byte type,
	void const *prefix,
	word prefix_size)
{
	packer->sender = sender;
	packer->type = type;
	packer->count = 0;
	packer->prefix_size = prefix_size;
	packer->size = prefix_size;
	packer->room = 0;
	if (prefix_size)
		csmemcpy(packer->message.data, prefix, prefix_size);
}

/* the message packed so far, into the batch */
static void distributed_packer_flush(
	struct distributed_packer *packer)
{
	word size = (word)(sizeof(packer->message.header) + packer->size);

	if (!packer->count)
		return;
	distributed_fill_header(&packer->message, packer->type, packer->count, size);
	distributed_batch_add(packer->sender, &packer->message, size, 0);
	packer->count = 0;
	packer->size = packer->prefix_size;
}

/* an entry into the message, which goes into the batch first when the
entry would not fit it (and a batch full goes first then) */
static void distributed_packer_add(
	struct distributed_packer *packer,
	void const *entry,
	word entry_size)
{
	if (packer->count && (packer->size + entry_size > packer->room || packer->count >= 255))
		distributed_packer_flush(packer);
	if (!packer->count)
		packer->room = MIN(distributed_batch_room(packer->sender, (word)(packer->prefix_size + entry_size)),
			(word)sizeof(packer->message.data));
	if (packer->size + entry_size > packer->room)
		return;
	csmemcpy(packer->message.data + packer->size, entry, entry_size);
	packer->size += entry_size;
	packer->count++;
}

/* the bytes of what a message says, in order: written, and read no further
than its end (FALSE past it) */
static byte *distributed_put(
	byte *cursor,
	void const *data,
	short size)
{
	csmemcpy(cursor, data, size);
	return cursor + size;
}

static boolean distributed_take(
	byte const **cursor,
	byte const *end,
	void *data,
	short size)
{
	if (end - *cursor < size)
		return FALSE;
	csmemcpy(data, *cursor, size);
	*cursor += size;
	return TRUE;
}

struct player_datum *distributed_player(
	short player_index)
{
	struct player_datum *player;

	if (player_index < 0 || player_index >= player_data->maximum_count)
		return NULL;
	player = (struct player_datum *)((byte *)player_data->data + player_index * player_data->size);
	return player->identifier ? player : NULL;
}

byte distributed_player_to_byte(
	long player_index)
{
	return player_index != NONE ? (byte)DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index) : NO_PLAYER;
}

long distributed_player_from_byte(
	byte player_index)
{
	struct player_datum *player = player_index != NO_PLAYER ? distributed_player(player_index) : NULL;

	return player ? DATUM_INDEX_NEW(player_index, player->identifier) : NONE;
}

boolean distributed_player_is_local(
	long player_index)
{
	struct player_datum *player = player_index != NONE ? player_try_and_get(player_index) : NULL;

	return player && player->local_player_index != NONE;
}

long distributed_living_unit(
	struct player_datum const *player)
{
	if (!player || player->unit_index == NONE || !object_try_and_get(player->unit_index) ||
		TEST_FLAG(object_get(player->unit_index)->object.damage_flags, _object_dead_bit))
	{
		return NONE;
	}
	return player->unit_index;
}

boolean distributed_machine_has_player(
	long machine_index,
	short player_index)
{
	long *player_list = machine_get_player_list(machine_index);
	short local_player_index;

	for (local_player_index = 0; local_player_index < MAXIMUM_LOCAL_PLAYERS; local_player_index++)
	{
		if (player_list[local_player_index] != NONE &&
			DATUM_INDEX_TO_ABSOLUTE_INDEX(player_list[local_player_index]) == player_index)
		{
			return TRUE;
		}
	}
	return FALSE;
}

/* whether the machine's players are this machine's (the host's own machine
is in the game as its clients are) */
static boolean distributed_machine_is_local(
	long machine_index)
{
	long *player_list = machine_get_player_list(machine_index);
	short local_player_index;

	for (local_player_index = 0; local_player_index < MAXIMUM_LOCAL_PLAYERS; local_player_index++)
	{
		struct player_datum *player = player_list[local_player_index] != NONE ?
			player_try_and_get(player_list[local_player_index]) : NULL;

		if (player && player->local_player_index != NONE)
			return TRUE;
	}
	return FALSE;
}

short distributed_client_machines(
	long *machine_indices,
	short maximum)
{
	short index;

	/* (found once a tick: the machines do not change during one) */
	if (!distributed_machines.valid)
	{
		short count = network_distributed_server_machines(distributed_machines.indices,
			HALO_PORT_MAXIMUM_NETWORK_MACHINES);

		distributed_machines.count = 0;
		for (index = 0; index < count; index++)
		{
			if (distributed_machines.indices[index] >= 0 &&
				distributed_machines.indices[index] < HALO_PORT_MAXIMUM_NETWORK_MACHINES &&
				!distributed_machine_is_local(distributed_machines.indices[index]))
			{
				distributed_machines.indices[distributed_machines.count++] = distributed_machines.indices[index];
			}
		}
		distributed_machines.valid = distributed_machines.in_tick;
	}
	for (index = 0; index < distributed_machines.count && index < maximum; index++)
		machine_indices[index] = distributed_machines.indices[index];
	return index;
}

/* (the host) a machine has loaded the game: if it is new at its index (its
players are not those the index had), none of the old machine's state, and
TRUE (it has none of the game's either); FALSE for one that asks again */
static boolean distributed_machine_loaded(
	long machine_index)
{
	long *player_list;
	short type;
	short player_index;

	if (machine_index < 0 || machine_index >= HALO_PORT_MAXIMUM_NETWORK_MACHINES)
		return FALSE;
	player_list = machine_get_player_list(machine_index);
	if (!csmemcmp(distributed_machine_players[machine_index], player_list,
		sizeof(distributed_machine_players[machine_index])))
	{
		return FALSE;
	}
	csmemcpy(distributed_machine_players[machine_index], player_list,
		sizeof(distributed_machine_players[machine_index]));
	for (type = 0; type < NUMBER_OF_DISTRIBUTED_MESSAGES; type++)
		distributed_received_times[machine_index][type] = NONE;
	csmemset(&distributed_round_trips[machine_index], 0, sizeof(distributed_round_trips[machine_index]));
	csmemset(distributed_viewers[machine_index], 0, sizeof(distributed_viewers[machine_index]));
	csmemset(&distributed_client_clocks[machine_index], 0, sizeof(distributed_client_clocks[machine_index]));
	distributed_client_clocks[machine_index].window_tick = NONE;
	csmemset(&distributed_client_identities[machine_index], 0, sizeof(distributed_client_identities[machine_index]));
	for (player_index = 0; player_index < MAXIMUM_TRACKED_PLAYERS; player_index++)
		distributed_seen[machine_index][player_index] = FALSE;
	return TRUE;
}

long distributed_latest_host_time(
	void)
{
	return distributed_host_time;
}

real distributed_own_round_trip_ticks(
	void)
{
	return distributed_own_round_trip;
}

long distributed_player_machine(
	short player_index)
{
	return player_index >= 0 && player_index < MAXIMUM_TRACKED_PLAYERS ?
		distributed_player_machines[player_index] : NONE;
}

boolean distributed_machine_sees_player(
	long machine_index,
	short player_index)
{
	return machine_index >= 0 && machine_index < HALO_PORT_MAXIMUM_NETWORK_MACHINES &&
		player_index >= 0 && player_index < MAXIMUM_TRACKED_PLAYERS &&
		TEST_FLAG(distributed_sends[machine_index][player_index], _distributed_send_sees_bit);
}

long distributed_player_ping(
	short player_index)
{
	long machine_index;

	if (player_index < 0 || player_index >= MAXIMUM_TRACKED_PLAYERS || !distributed_player(player_index))
		return NONE;
	if (game_connection() == _game_connection_network_client)
		return distributed_player_pings[player_index] == UNKNOWN_PING ? NONE : (long)distributed_player_pings[player_index];
	if (game_connection() != _game_connection_network_server)
		return NONE;
	/* (the host: its own players' none, a client's its machine's) */
	machine_index = distributed_player_machines[player_index];
	if (machine_index == NONE)
		return 0;
	if (machine_index < 0 || machine_index >= HALO_PORT_MAXIMUM_NETWORK_MACHINES ||
		!distributed_round_trips[machine_index].valid)
	{
		return NONE;
	}
	return (long)(distributed_round_trips[machine_index].average * 1000.0f / TICKS_PER_SECOND + 0.5f);
}

real distributed_machine_round_trip_ticks(
	long machine_index)
{
	if (machine_index < 0 || machine_index >= HALO_PORT_MAXIMUM_NETWORK_MACHINES ||
		!distributed_round_trips[machine_index].valid)
	{
		return (real)DEFAULT_ROUND_TRIP_TICKS;
	}
	return distributed_round_trips[machine_index].average + 2.0f * distributed_round_trips[machine_index].deviation;
}

/* ---------- units */

static void distributed_state_from_player(
	short player_index,
	struct distributed_unit_state *state)
{
	struct player_datum *player = distributed_player(player_index);
	long unit_index = distributed_living_unit(player);

	csmemset(state, 0, sizeof(*state));
	state->player_index = (byte)player_index;
	state->unit_index = NONE;
	state->vehicle_index = NONE;
	state->seat_index = NONE;
	if (unit_index != NONE)
	{
		struct unit_datum *unit = unit_get(unit_index);
		struct damage_network_state damage;
		real camouflage = unit->unit.active_camouflage;

		state->unit_index = unit_index;
		SET_FLAG(state->flags, _distributed_unit_alive_bit, TRUE);
		SET_FLAG(state->flags, _distributed_unit_placed_bit, unit->object.parent_object_index == NONE);
		if (unit->object.parent_object_index != NONE && unit->unit.parent_seat_index != NONE)
		{
			state->vehicle_index = unit->object.parent_object_index;
			state->seat_index = unit->unit.parent_seat_index;
		}
		state->position = unit->object.position;
		distributed_vector_pack(&unit->object.translational_velocity, DISTRIBUTED_VELOCITY_SCALE, &state->velocity);
		distributed_vector_pack(&unit->object.forward, DISTRIBUTED_UNIT_SCALE, &state->forward);
		distributed_vector_pack(&unit->object.up, DISTRIBUTED_UNIT_SCALE, &state->up);
		damage_get_network_state(unit_index, &damage);
		SET_FLAG(state->flags, _distributed_unit_shield_depleted_bit, damage.shield_depleted);
		SET_FLAG(state->flags, _distributed_unit_shield_charging_bit, damage.shield_charging);
		SET_FLAG(state->flags, _distributed_unit_shield_over_charging_bit, damage.shield_over_charging);
		state->body_vitality = distributed_vitality_pack(damage.body_vitality);
		state->shield_vitality = distributed_vitality_pack(damage.shield_vitality);
		state->current_body_damage = distributed_vitality_pack(damage.current_body_damage);
		state->recent_body_damage = distributed_vitality_pack(damage.recent_body_damage);
		state->current_shield_damage = distributed_vitality_pack(damage.current_shield_damage);
		state->recent_shield_damage = distributed_vitality_pack(damage.recent_shield_damage);
		csmemcpy(state->powerup_durations, player->powerup_durations, sizeof(state->powerup_durations));
		SET_FLAG(state->unit_flags, _distributed_unit_camouflaged_bit,
			TEST_FLAG(unit->unit.flags, _unit_active_camouflaged_bit));
		SET_FLAG(state->unit_flags, _distributed_unit_super_camouflaged_bit,
			TEST_FLAG(unit->unit.flags, _unit_super_camouflaged_bit));
		camouflage = camouflage > 1.0f ? 1.0f : camouflage < 0.0f ? 0.0f : camouflage;
		state->active_camouflage = (byte)(long)floor(camouflage * 255.0f + 0.5f);
		/* (the host) a client's player where the client had them: at which of
		its ticks (with the ticks run since) */
		if (game_connection() == _game_connection_network_server && player_index < MAXIMUM_TRACKED_PLAYERS &&
			distributed_predictions[player_index].taken_host_time != NONE &&
			game_time_get() - distributed_predictions[player_index].taken_host_time < PREDICTION_ECHO_TICKS)
		{
			SET_FLAG(state->flags, _distributed_unit_predicted_bit, TRUE);
			state->predicted_time = (short)(word)(distributed_predictions[player_index].taken_time +
				game_time_get() - distributed_predictions[player_index].taken_host_time);
		}
	}
	state->killing_player_index = NO_PLAYER;
	if (unit_index == NONE && player_index < MAXIMUM_TRACKED_PLAYERS && distributed_deaths[player_index].valid)
	{
		struct distributed_death const *death = &distributed_deaths[player_index];

		if (death->killing_player_index != NONE)
			state->killing_player_index = (byte)death->killing_player_index;
		SET_FLAG(state->flags, _distributed_unit_friendly_fire_bit, death->friendly_fire);
		SET_FLAG(state->flags, _distributed_unit_killed_by_vehicle_bit, death->killed_by_vehicle);
	}
}

/* the state's bytes on the wire (at most DISTRIBUTED_UNIT_STATE_MAXIMUM_SIZE):
its player, flags and parts, then (alive) its unit, the vehicle and seat it
rides, (placed) where it is, how fast it moves and which way it faces,
(predicted) the client's tick, its shields and health, the damage they
show, its powerups and camouflage, or (dead) who killed it; what is its
default is left out (distributed_unit_state_read). The predicted tick is
its own client's alone (own). */
static word distributed_unit_state_write(
	struct distributed_unit_state const *state,
	boolean own,
	byte *buffer)
{
	byte flags = state->flags;
	byte parts = 0;
	byte *cursor = buffer + DISTRIBUTED_UNIT_STATE_MINIMUM_SIZE;

	if (!own)
		SET_FLAG(flags, _distributed_unit_predicted_bit, FALSE);
	if (TEST_FLAG(flags, _distributed_unit_alive_bit))
	{
		cursor = distributed_put(cursor, &state->unit_index, sizeof(state->unit_index));
		if (state->vehicle_index != NONE)
		{
			SET_FLAG(parts, _distributed_unit_part_riding_bit, TRUE);
			cursor = distributed_put(cursor, &state->vehicle_index, sizeof(state->vehicle_index));
			cursor = distributed_put(cursor, &state->seat_index, sizeof(state->seat_index));
		}
		if (TEST_FLAG(flags, _distributed_unit_placed_bit))
		{
			cursor = distributed_put(cursor, &state->position, sizeof(state->position));
			cursor = distributed_put(cursor, &state->velocity, sizeof(state->velocity));
			cursor = distributed_put(cursor, &state->forward, sizeof(state->forward));
			if (state->up.i || state->up.j || state->up.k != (short)DISTRIBUTED_UNIT_SCALE)
			{
				SET_FLAG(parts, _distributed_unit_part_up_bit, TRUE);
				cursor = distributed_put(cursor, &state->up, sizeof(state->up));
			}
		}
		if (TEST_FLAG(flags, _distributed_unit_predicted_bit))
			cursor = distributed_put(cursor, &state->predicted_time, sizeof(state->predicted_time));
		cursor = distributed_put(cursor, &state->body_vitality, sizeof(state->body_vitality));
		cursor = distributed_put(cursor, &state->shield_vitality, sizeof(state->shield_vitality));
		if (state->current_body_damage || state->recent_body_damage || state->current_shield_damage ||
			state->recent_shield_damage)
		{
			SET_FLAG(parts, _distributed_unit_part_damage_bit, TRUE);
			cursor = distributed_put(cursor, &state->current_body_damage, sizeof(state->current_body_damage));
			cursor = distributed_put(cursor, &state->recent_body_damage, sizeof(state->recent_body_damage));
			cursor = distributed_put(cursor, &state->current_shield_damage, sizeof(state->current_shield_damage));
			cursor = distributed_put(cursor, &state->recent_shield_damage, sizeof(state->recent_shield_damage));
		}
		{
			short powerup_index;

			for (powerup_index = 0; powerup_index < NUMBER_OF_PLAYER_POWERUPS; powerup_index++)
			{
				if (state->powerup_durations[powerup_index])
					break;
			}
			if (powerup_index < NUMBER_OF_PLAYER_POWERUPS)
			{
				SET_FLAG(parts, _distributed_unit_part_powerups_bit, TRUE);
				cursor = distributed_put(cursor, state->powerup_durations, sizeof(state->powerup_durations));
			}
		}
		if (state->unit_flags || state->active_camouflage)
		{
			SET_FLAG(parts, _distributed_unit_part_camouflage_bit, TRUE);
			cursor = distributed_put(cursor, &state->unit_flags, sizeof(state->unit_flags));
			cursor = distributed_put(cursor, &state->active_camouflage, sizeof(state->active_camouflage));
		}
	}
	else if (state->killing_player_index != NO_PLAYER)
	{
		SET_FLAG(parts, _distributed_unit_part_killer_bit, TRUE);
		cursor = distributed_put(cursor, &state->killing_player_index, sizeof(state->killing_player_index));
	}
	buffer[0] = state->player_index;
	buffer[1] = flags;
	buffer[2] = parts;
	return (word)(cursor - buffer);
}

/* a state from its bytes on the wire, no further than end: the bytes it
took, 0 for none that make one */
static word distributed_unit_state_read(
	byte const *buffer,
	byte const *end,
	struct distributed_unit_state *state)
{
	byte const *cursor = buffer;
	byte parts;

	csmemset(state, 0, sizeof(*state));
	state->unit_index = NONE;
	state->vehicle_index = NONE;
	state->seat_index = NONE;
	state->killing_player_index = NO_PLAYER;
	state->up.k = (short)DISTRIBUTED_UNIT_SCALE;
	if (!distributed_take(&cursor, end, &state->player_index, sizeof(state->player_index)) ||
		!distributed_take(&cursor, end, &state->flags, sizeof(state->flags)) ||
		!distributed_take(&cursor, end, &parts, sizeof(parts)))
	{
		return 0;
	}
	if (TEST_FLAG(state->flags, _distributed_unit_alive_bit))
	{
		if (!distributed_take(&cursor, end, &state->unit_index, sizeof(state->unit_index)))
			return 0;
		if (TEST_FLAG(parts, _distributed_unit_part_riding_bit) &&
			(!distributed_take(&cursor, end, &state->vehicle_index, sizeof(state->vehicle_index)) ||
				!distributed_take(&cursor, end, &state->seat_index, sizeof(state->seat_index))))
		{
			return 0;
		}
		if (TEST_FLAG(state->flags, _distributed_unit_placed_bit) &&
			(!distributed_take(&cursor, end, &state->position, sizeof(state->position)) ||
				!distributed_take(&cursor, end, &state->velocity, sizeof(state->velocity)) ||
				!distributed_take(&cursor, end, &state->forward, sizeof(state->forward)) ||
				(TEST_FLAG(parts, _distributed_unit_part_up_bit) &&
					!distributed_take(&cursor, end, &state->up, sizeof(state->up)))))
		{
			return 0;
		}
		if (TEST_FLAG(state->flags, _distributed_unit_predicted_bit) &&
			!distributed_take(&cursor, end, &state->predicted_time, sizeof(state->predicted_time)))
		{
			return 0;
		}
		if (!distributed_take(&cursor, end, &state->body_vitality, sizeof(state->body_vitality)) ||
			!distributed_take(&cursor, end, &state->shield_vitality, sizeof(state->shield_vitality)))
		{
			return 0;
		}
		if (TEST_FLAG(parts, _distributed_unit_part_damage_bit) &&
			(!distributed_take(&cursor, end, &state->current_body_damage, sizeof(state->current_body_damage)) ||
				!distributed_take(&cursor, end, &state->recent_body_damage, sizeof(state->recent_body_damage)) ||
				!distributed_take(&cursor, end, &state->current_shield_damage, sizeof(state->current_shield_damage)) ||
				!distributed_take(&cursor, end, &state->recent_shield_damage, sizeof(state->recent_shield_damage))))
		{
			return 0;
		}
		if (TEST_FLAG(parts, _distributed_unit_part_powerups_bit) &&
			!distributed_take(&cursor, end, state->powerup_durations, sizeof(state->powerup_durations)))
		{
			return 0;
		}
		if (TEST_FLAG(parts, _distributed_unit_part_camouflage_bit) &&
			(!distributed_take(&cursor, end, &state->unit_flags, sizeof(state->unit_flags)) ||
				!distributed_take(&cursor, end, &state->active_camouflage, sizeof(state->active_camouflage))))
		{
			return 0;
		}
	}
	else if (TEST_FLAG(parts, _distributed_unit_part_killer_bit) &&
		!distributed_take(&cursor, end, &state->killing_player_index, sizeof(state->killing_player_index)))
	{
		return 0;
	}
	return (word)(cursor - buffer);
}

/* moves the unit toward the state (at position) if it is further than
tolerance from it: within blend_distance part of the way, further all of it,
no faster than maximum_speed across and up and maximum_fall_speed down (0
for any); FALSE for a state that cannot be */
static boolean distributed_apply_state(
	long unit_index,
	struct distributed_unit_state const *state,
	real_point3d const *position,
	real tolerance,
	real blend_distance,
	real maximum_speed,
	real maximum_fall_speed)
{
	struct object_datum *object = object_get(unit_index);
	real_vector3d error;
	real_vector3d velocity;
	real_vector3d forward;
	real_vector3d up;

	distributed_vector_unpack(&state->velocity, DISTRIBUTED_VELOCITY_SCALE, &velocity);
	distributed_vector_unpack(&state->forward, DISTRIBUTED_UNIT_SCALE, &forward);
	distributed_vector_unpack(&state->up, DISTRIBUTED_UNIT_SCALE, &up);
	if (!distributed_point_valid(position, UNIT_WORLD_BOUND) || !distributed_axes_make_valid(&forward, &up))
		return FALSE;
	if (maximum_speed > 0.0f)
	{
		real rise = velocity.k > 0.0f ? velocity.k : 0.0f;
		real speed = (real)sqrt(velocity.i * velocity.i + velocity.j * velocity.j + rise * rise);

		if (speed > maximum_speed)
		{
			velocity.i *= maximum_speed / speed;
			velocity.j *= maximum_speed / speed;
			if (velocity.k > 0.0f)
				velocity.k *= maximum_speed / speed;
		}
		if (maximum_fall_speed > 0.0f && velocity.k < -maximum_fall_speed)
			velocity.k = -maximum_fall_speed;
	}
	error.i = position->x - object->object.position.x;
	error.j = position->y - object->object.position.y;
	error.k = position->z - object->object.position.z;
	if (error.i * error.i + error.j * error.j + error.k * error.k <= tolerance * tolerance)
		return TRUE;
	if (network_objects_reconcile(unit_index, position, &forward, &up, &velocity, NULL, blend_distance))
		distributed_statistics.corrections++;
	return TRUE;
}

/* (a client) where its own players' units are this tick, noted */
static void distributed_note_own_positions(
	void)
{
	short local_player_index;

	for (local_player_index = 0; local_player_index < MAXIMUM_LOCAL_PLAYERS; local_player_index++)
	{
		long player_index = local_player_get_player_index(local_player_index);
		struct player_datum *player = player_index != NONE ? player_try_and_get(player_index) : NULL;
		long unit_index = distributed_living_unit(player);
		struct distributed_own_position *own =
			&distributed_own_positions[local_player_index][game_time_get() & (OWN_POSITION_TICKS - 1)];

		/* (not riding: a rider's position is its seat's) */
		if (unit_index != NONE && object_get(unit_index)->object.parent_object_index != NONE)
			unit_index = NONE;
		own->time = game_time_get();
		own->unit_index = unit_index;
		if (unit_index != NONE)
			own->position = object_get(unit_index)->object.position;
	}
}

/* (a client) its own player's unit where the host has it, if that is
further than a tolerance from where this machine had it at the tick the
host has it at (its own prediction come back): moved by the difference,
keeping what it has done since (no rubber band a round trip long); without
that tick, from where it is */
static void distributed_correct_own_unit(
	struct player_datum const *player,
	long unit_index,
	struct distributed_unit_state const *state)
{
	short local_player_index = player->local_player_index;
	real_point3d position = state->position;

	if (TEST_FLAG(state->flags, _distributed_unit_predicted_bit) &&
		local_player_index >= 0 && local_player_index < MAXIMUM_LOCAL_PLAYERS)
	{
		long now = game_time_get();
		long time = now - (long)(word)((word)now - (word)state->predicted_time);
		struct distributed_own_position *own =
			&distributed_own_positions[local_player_index][time & (OWN_POSITION_TICKS - 1)];

		if (own->time == time && own->unit_index == unit_index)
		{
			struct object_datum *object = object_get(unit_index);
			real_vector3d error;
			short index;

			/* (how long the host takes to have this machine's players, as
			the round trip is smoothed on the host) */
			if (distributed_own_round_trip <= 0.0f)
				distributed_own_round_trip = (real)(now - time);
			else
				distributed_own_round_trip += ((real)(now - time) - distributed_own_round_trip) / 8.0f;
			error.i = state->position.x - own->position.x;
			error.j = state->position.y - own->position.y;
			error.k = state->position.z - own->position.z;
			if (!(error.i * error.i + error.j * error.j + error.k * error.k >
				LOCAL_CORRECTION_TOLERANCE * LOCAL_CORRECTION_TOLERANCE))
			{
				return;
			}
			{
				real_point3d before = object->object.position;

				position.x = before.x + error.i;
				position.y = before.y + error.j;
				position.z = before.z + error.k;
				distributed_apply_state(unit_index, state, &position, LOCAL_CORRECTION_TOLERANCE, 0.0f, 0.0f, 0.0f);
				/* (the ticks noted since moved as it was, not corrected again) */
				error.i = object->object.position.x - before.x;
				error.j = object->object.position.y - before.y;
				error.k = object->object.position.z - before.z;
				for (index = 0; index < OWN_POSITION_TICKS; index++)
				{
					struct distributed_own_position *noted = &distributed_own_positions[local_player_index][index];

					if (noted->unit_index == unit_index)
					{
						noted->position.x += error.i;
						noted->position.y += error.j;
						noted->position.z += error.k;
					}
				}
				return;
			}
		}
	}
	distributed_apply_state(unit_index, state, &position, LOCAL_CORRECTION_TOLERANCE, 0.0f, 0.0f, 0.0f);
}

/* (a client) its own players' units, to the host */
static void distributed_client_send_predictions(
	void)
{
	struct data_iterator iterator;
	struct player_datum *player;

	distributed_packer_begin(&distributed_packer, HOST_SENDER, _distributed_message_player_prediction, NULL, 0);
	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		long unit_index = distributed_living_unit(player);
		struct distributed_unit_state state;
		byte entry[DISTRIBUTED_UNIT_STATE_MAXIMUM_SIZE];

		/* a client speaks for its own players only, where they are */
		if (player->local_player_index == NONE || unit_index == NONE ||
			object_get(unit_index)->object.parent_object_index != NONE)
		{
			continue;
		}
		distributed_state_from_player((short)DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.datum_index), &state);
		distributed_packer_add(&distributed_packer, entry, distributed_unit_state_write(&state, TRUE, entry));
	}
	distributed_packer_flush(&distributed_packer);
}

/* (the host) a client's own players: the latest of each, taken at the next
tick */
static void distributed_handle_predictions(
	long machine_index,
	long time,
	byte const *data,
	byte const *end,
	short count)
{
	short index;

	for (index = 0; index < count; index++)
	{
		struct distributed_unit_state state;
		word size = distributed_unit_state_read(data, end, &state);

		if (!size)
			break;
		data += size;
		if (state.player_index >= MAXIMUM_TRACKED_PLAYERS ||
			!distributed_machine_has_player(machine_index, state.player_index) ||
			!TEST_FLAG(state.flags, _distributed_unit_alive_bit) ||
			!TEST_FLAG(state.flags, _distributed_unit_placed_bit))
		{
			continue;
		}
		distributed_predictions[state.player_index].valid = TRUE;
		distributed_predictions[state.player_index].time = time;
		distributed_predictions[state.player_index].state = state;
	}
}

/* (the host) how fast a client's player's unit on foot may go, a tick, and
how high (distributed_on_foot_bound) */
struct distributed_on_foot_bound
{
	/* across and up (world units a tick) */
	real speed;
	/* the highest the host has had it since it last had it on the ground,
	and how far above where it had it then it may be (world units) */
	real top_height;
	real rise;
	/* ... in the air, the speed up it may have left the ground at and the
	height it may have had then, and the ticks the host's copy has been in
	the air (none on the ground): as high as a thing thrown up so, falling
	since (distributed_on_foot_ceiling) */
	real rise_speed;
	real rise_base;
	long airborne_ticks;
};

/* (the host) how fast a client's player's unit on foot may go, a tick, and
how high: across and up twice as fast as the player runs and jumps (their
speed in the game, double speed), or as fast as the host's own ticks sent
its copy lately (an explosion's throw, which the client learns of a round
trip late), whichever is more, no more than MAXIMUM_PREDICTED_SPEED; down
as fast as that and a fall from the highest it has had it since it was on
the ground (distributed_on_foot_fall_speed); up no higher above where it
last had it on the ground than a jump takes it (or a run up a slope, and
the legs drawn up) and what its ticks threw it up. What its ticks sent it
is its speed less as much as the velocity it took of the client was faster
than the player goes of their own, and up what its tick added to that, but
a jump's: a client that says it goes faster than that gains nothing by it
(its gravity on a copy said to hover and fall, its turn at a wall), nor
any height. */
static void distributed_on_foot_bound(
	short player_index,
	long unit_index,
	struct distributed_on_foot_bound *bound)
{
	struct distributed_host_speed *noted = &distributed_host_speeds[player_index];
	struct object_datum *object = object_get(unit_index);
	struct biped_datum *biped = (struct biped_datum *)object_try_and_get_and_verify_type(unit_index,
		_object_mask_biped);
	struct player_datum *player = distributed_player(player_index);
	struct game_globals *globals = scenario_get_game_globals();
	long now = game_time_get();
	real_vector3d const *velocity = &object->object.translational_velocity;
	real host_speed = (real)sqrt(velocity->i * velocity->i + velocity->j * velocity->j + velocity->k * velocity->k);
	real taken_speed = (real)sqrt(noted->velocity.i * noted->velocity.i + noted->velocity.j * noted->velocity.j +
		noted->velocity.k * noted->velocity.k);
	real run_speed = 0.0f;
	real jump_speed = 0.0f;
	real crouch_rise = 0.0f;
	real base_speed;
	real added_rise;
	real rise_speed;

	/* (a unit new on foot, a new life or a ride's end: all its speed its
	ticks', from where it is) */
	if (noted->unit_index != unit_index)
	{
		noted->unit_index = unit_index;
		noted->velocity.i = 0.0f;
		noted->velocity.j = 0.0f;
		noted->velocity.k = 0.0f;
		noted->grounded = FALSE;
		noted->flight_speed = 0.0f;
		noted->flight_rise = 0.0f;
		noted->ground_height = object->object.position.z;
		noted->top_height = object->object.position.z;
		noted->ground_time = now;
		taken_speed = 0.0f;
	}
	/* (the most of each span) */
	if (now < noted->time || now - noted->time >= 2 * PREDICTION_SPEED_TICKS)
	{
		noted->time = now;
		noted->speeds[0] = 0.0f;
		noted->speeds[1] = 0.0f;
		noted->rises[0] = 0.0f;
		noted->rises[1] = 0.0f;
	}
	else if (now - noted->time >= PREDICTION_SPEED_TICKS)
	{
		noted->time = now;
		noted->speeds[1] = noted->speeds[0];
		noted->speeds[0] = 0.0f;
		noted->rises[1] = noted->rises[0];
		noted->rises[0] = 0.0f;
	}
	/* (as bipeds.c moves a player: forward or back and sideways at once) */
	if (globals && globals->player_information.count > 0)
	{
		struct game_globals_player_information *information = TAG_BLOCK_GET_ELEMENT(&globals->player_information, 0,
			struct game_globals_player_information);
		real forward = MAX(information->run_forward_speed, information->run_backward_speed);
		real sideways = information->run_sideways_speed;

		run_speed = (real)sqrt(forward * forward + sideways * sideways) / TICKS_PER_SECOND;
		if (game_players_are_double_speed() && information->double_speed_multiplier > 1.0f)
			run_speed *= information->double_speed_multiplier;
	}
	if (player && player->speed_multiplier > 1.0f)
		run_speed *= player->speed_multiplier;
	if (biped)
	{
		struct biped_definition *definition = biped_definition_get(biped->definition_index);

		jump_speed = definition->biped.jump_velocity;
		crouch_rise = definition->biped.collision_height_standing - definition->biped.collision_height_crouching;
	}
	base_speed = PREDICTED_ON_FOOT_SPEED_SCALE * (real)sqrt(run_speed * run_speed + jump_speed * jump_speed);
	/* (so written that a tag's speed not a number is none) */
	if (!(base_speed >= 0.0f))
		base_speed = 0.0f;
	if (!(jump_speed >= 0.0f))
		jump_speed = 0.0f;
	if (!(crouch_rise >= 0.0f))
		crouch_rise = 0.0f;
	/* (what its tick sent it, noted; so written that a speed not a number
	is none) */
	if (taken_speed > base_speed)
		host_speed -= taken_speed - base_speed;
	added_rise = MAX(velocity->k, 0.0f) - MAX(noted->velocity.k, 0.0f);
	if (noted->grounded)
		added_rise -= jump_speed;
	if (host_speed > noted->speeds[0])
		noted->speeds[0] = host_speed;
	if (added_rise > noted->rises[0])
		noted->rises[0] = added_rise;
	/* (how high: where it is on the ground, or the highest since; and in
	the air, a throw's speeds kept till it lands, a few seconds at most) */
	noted->grounded = !biped || !TEST_FLAG(biped->biped.flags, _biped_airborne_bit);
	if (noted->grounded)
	{
		noted->ground_height = object->object.position.z;
		noted->ground_time = now;
	}
	if (noted->grounded || object->object.position.z > noted->top_height)
		noted->top_height = object->object.position.z;
	if (noted->grounded || biped->biped.airborne_ticks >= SCHAR_MAX)
	{
		noted->flight_speed = 0.0f;
		noted->flight_rise = 0.0f;
	}
	else
	{
		noted->flight_speed = MAX(noted->flight_speed, MAX(noted->speeds[0], noted->speeds[1]));
		noted->flight_rise = MAX(noted->flight_rise, MAX(noted->rises[0], noted->rises[1]));
	}
	bound->speed = MAX(base_speed, MAX(MAX(noted->speeds[0], noted->speeds[1]), noted->flight_speed));
	/* (some: none is no bound, distributed_apply_state) */
	bound->speed = MAX(bound->speed, PREDICTED_ON_FOOT_SPEED_MARGIN);
	bound->speed = MIN(bound->speed, MAXIMUM_PREDICTED_SPEED);
	bound->top_height = noted->top_height;
	/* (a jump's rise, as fast up as it or a run up a slope is: its speed
	squared over twice gravity) */
	rise_speed = MAX(MAX(noted->rises[0], noted->rises[1]), noted->flight_rise);
	/* (a throw of it on the ground was noted less a jump, which its own
	client may have made as it came: a jump's again on top) */
	if (rise_speed > 0.0f)
		rise_speed += jump_speed;
	rise_speed += MAX(jump_speed, run_speed);
	bound->rise = UNIT_WORLD_BOUND;
	if (biped && global_gravity > 0.0f)
		bound->rise = rise_speed * rise_speed / (2.0f * global_gravity) + crouch_rise + PREDICTED_RISE_TOLERANCE;
	bound->rise_speed = rise_speed;
	bound->rise_base = crouch_rise + PREDICTED_RISE_TOLERANCE;
	bound->airborne_ticks = noted->grounded ? 0 : now - noted->ground_time;
	if (!(bound->rise >= 0.0f))
		bound->rise = UNIT_WORLD_BOUND;
}

/* (the host) how fast a client's player's unit on foot may fall at a
height (world units a tick): as fast as it goes across, and as fast as a
fall from the highest the host has had it since it was on the ground, no
more than MAXIMUM_PREDICTED_SPEED */
static real distributed_on_foot_fall_speed(
	struct distributed_on_foot_bound const *bound,
	real height)
{
	real drop = bound->top_height - height;
	real speed = bound->speed;

	if (drop > 0.0f && global_gravity > 0.0f)
		speed += (real)sqrt(2.0f * global_gravity * drop);
	return MIN(speed, MAXIMUM_PREDICTED_SPEED);
}

/* (the host) how far above where the host last had a client's player's
unit on the ground it may be: a jump's height (or a throw's), and in the
air no higher than a thing thrown up at that speed falls to since (the
ticks its copy has been in the air, less the client's round trip and
jitter, which are the client's ahead): no hovering in the air, nor coming
down slowly */
static real distributed_on_foot_ceiling(
	short player_index,
	struct distributed_on_foot_bound const *bound)
{
	real rise = bound->rise;
	/* (a round trip a client makes long gains it no more than this) */
	real ticks = (real)bound->airborne_ticks -
		MIN(distributed_machine_round_trip_ticks(distributed_player_machine(player_index)),
			(real)PREDICTION_CEILING_LEAD_TICKS) -
		(real)PREDICTION_JITTER_TICKS;

	if (bound->airborne_ticks > 0 && ticks > 0.0f && global_gravity > 0.0f)
	{
		real ceiling = bound->rise_base + bound->rise_speed * ticks - 0.5f * global_gravity * ticks * ticks;

		if (ceiling < rise)
			rise = ceiling;
	}
	return rise;
}

/* (the host) whether a client's player's unit on foot goes from one point
to the other in the ticks: across and up no further than at its speed, down
no further than it falls to there, a little more a tick (and a blend) */
static boolean distributed_on_foot_move_valid(
	struct distributed_on_foot_bound const *bound,
	real_point3d const *from,
	real_point3d const *to,
	long ticks)
{
	real dx = to->x - from->x;
	real dy = to->y - from->y;
	real dz = to->z - from->z;
	real rise = dz > 0.0f ? dz : 0.0f;
	real reach = MIN(bound->speed + PREDICTED_ON_FOOT_SPEED_MARGIN, MAXIMUM_PREDICTED_SPEED) * (real)ticks +
		HOST_BLEND_DISTANCE;
	real fall_reach = MIN(distributed_on_foot_fall_speed(bound, to->z) + PREDICTED_ON_FOOT_SPEED_MARGIN,
		MAXIMUM_PREDICTED_SPEED) * (real)ticks + HOST_BLEND_DISTANCE;

	/* (so written that a position not a number is not within) */
	return dx * dx + dy * dy + rise * rise <= reach * reach && -dz <= fall_reach;
}

/* (the host) a client's player's prediction of its unit on foot, taken
where it says within a tolerance (distributed_apply_predictions) */
static void distributed_take_prediction(
	short player_index,
	long unit_index,
	struct distributed_on_foot_bound const *bound)
{
	struct distributed_unit_state const *state = &distributed_predictions[player_index].state;
	struct object_datum *object = object_get(unit_index);
	long now = game_time_get();
	real dx = state->position.x - object->object.position.x;
	real dy = state->position.y - object->object.position.y;
	real dz = state->position.z - object->object.position.z;

	if (!(dx * dx + dy * dy + dz * dz <= HOST_ACCEPT_TOLERANCE * HOST_ACCEPT_TOLERANCE))
		return;
	if (distributed_accepted[player_index].valid)
	{
		long ticks = MIN(distributed_predictions[player_index].time - distributed_accepted[player_index].time,
			now - distributed_accepted[player_index].host_time + PREDICTION_JITTER_TICKS);

		/* (the host's own unit moved further than a unit moves since the
		last taken, which only the host moves it by: taken from where it
		is, a new anchor, and its height where it starts) */
		if (!distributed_on_foot_move_valid(bound, &distributed_accepted[player_index].taken_host_position,
			&object->object.position, now - distributed_accepted[player_index].taken_host_time))
		{
			distributed_accepted[player_index].valid = FALSE;
			distributed_host_speeds[player_index].ground_height = object->object.position.z;
			distributed_host_speeds[player_index].top_height = object->object.position.z;
			distributed_host_speeds[player_index].ground_time = now;
		}
		else if (ticks <= 0 || !distributed_on_foot_move_valid(bound, &distributed_accepted[player_index].position,
			&state->position, ticks))
		{
			return;
		}
	}
	/* (no higher above where the host last had it on the ground than a
	jump, or a throw its ticks gave it, takes it, and in the air falling:
	past that the host's copy falls as its ticks have it) */
	if (!(state->position.z - distributed_host_speeds[player_index].ground_height <=
		distributed_on_foot_ceiling(player_index, bound)))
	{
		return;
	}
	/* (the client told which of its ticks the host has it at) */
	if (distributed_apply_state(unit_index, state, &state->position, 0.0f, HOST_BLEND_DISTANCE, bound->speed,
		distributed_on_foot_fall_speed(bound, state->position.z)))
	{
		distributed_predictions[player_index].taken_time = distributed_predictions[player_index].time;
		distributed_predictions[player_index].taken_host_time = now;
		/* (a new anchor: the first, one a second on, or after a jump) */
		if (!distributed_accepted[player_index].valid ||
			now - distributed_accepted[player_index].host_time >= PREDICTION_ANCHOR_TICKS)
		{
			distributed_accepted[player_index].valid = TRUE;
			distributed_accepted[player_index].unit_index = unit_index;
			distributed_accepted[player_index].time = distributed_predictions[player_index].time;
			distributed_accepted[player_index].host_time = now;
			distributed_accepted[player_index].position = state->position;
		}
		distributed_accepted[player_index].taken_host_time = now;
		distributed_accepted[player_index].taken_host_position = object->object.position;
	}
}

/* (the host) the clients' players where they say, within a tolerance: a
little off closed by half, more put there. Each accepted moves the host's
unit, and the next is measured from there, so a client could take its
player HOST_ACCEPT_TOLERANCE further each tick: a prediction is also no
further from the anchor (one taken, a newer once a second) than the player
moves in the client's ticks since (distributed_on_foot_bound, and a little
more), those no more than the host's since and a little jitter, no higher
than it climbs or jumps; its velocity is no faster. A teleporter moves the
host's own unit too, further than it moves since the last taken: it is
measured from where the host has it only, a new anchor (as it is after a
new life, or a ride). */
static void distributed_apply_predictions(
	void)
{
	short player_index;

	for (player_index = 0; player_index < MAXIMUM_TRACKED_PLAYERS; player_index++)
	{
		struct distributed_unit_state const *state = &distributed_predictions[player_index].state;
		struct distributed_on_foot_bound bound;
		long unit_index;

		if (!distributed_predictions[player_index].valid && !distributed_accepted[player_index].valid &&
			distributed_host_speeds[player_index].unit_index == NONE)
		{
			continue;
		}
		unit_index = distributed_living_unit(distributed_player(player_index));
		if (unit_index != NONE && object_get(unit_index)->object.parent_object_index != NONE)
			unit_index = NONE;
		if (distributed_accepted[player_index].valid && distributed_accepted[player_index].unit_index != unit_index)
			distributed_accepted[player_index].valid = FALSE;
		/* (how fast it may go, a tick, what the host's tick did to its own
		copy noted before the client's word is taken; riding or dead, noted
		afresh on foot) */
		if (unit_index != NONE)
			distributed_on_foot_bound(player_index, unit_index, &bound);
		else
			distributed_host_speeds[player_index].unit_index = NONE;
		if (distributed_predictions[player_index].valid)
		{
			distributed_predictions[player_index].valid = FALSE;
			distributed_predictions[player_index].taken_host_time = NONE;
			/* (of the unit it has now: not one of a life before) */
			if (unit_index != NONE && unit_index == state->unit_index)
				distributed_take_prediction(player_index, unit_index, &bound);
		}
		/* (what the host's next tick starts from) */
		if (unit_index != NONE)
		{
			struct object_datum *object = object_get(unit_index);

			distributed_host_speeds[player_index].velocity = object->object.translational_velocity;
			if (object->object.position.z > distributed_host_speeds[player_index].top_height)
				distributed_host_speeds[player_index].top_height = object->object.position.z;
		}
	}
}

/* (a client) the host's word on a player's unit */
static void distributed_handle_unit_state(
	struct distributed_unit_state const *state)
{
	struct player_datum *player = distributed_player(state->player_index);
	long player_index;
	long unit_index;
	boolean alive = TEST_FLAG(state->flags, _distributed_unit_alive_bit);
	boolean local;

	if (!player)
		return;
	player_index = DATUM_INDEX_NEW(state->player_index, player->identifier);
	local = player->local_player_index != NONE;
	/* how it died, for when this machine's copy dies
	(network_distributed_player_killed) */
	if (state->player_index < MAXIMUM_TRACKED_PLAYERS)
	{
		struct distributed_death *death = &distributed_deaths[state->player_index];

		death->valid = !alive;
		death->killing_player_index = state->killing_player_index != NO_PLAYER ?
			state->killing_player_index : NONE;
		death->friendly_fire = TEST_FLAG(state->flags, _distributed_unit_friendly_fire_bit);
		death->killed_by_vehicle = TEST_FLAG(state->flags, _distributed_unit_killed_by_vehicle_bit);
	}
	unit_index = distributed_living_unit(player);
	if (!alive)
	{
		/* died on the host (who counts it; the damage that killed it,
		network_damage.c, usually kills it here first) */
		if (unit_index != NONE)
			unit_kill_no_statistics(unit_index);
		return;
	}
	/* spawned on the host: the host's unit is the player's here too, once
	this machine has it (network_objects.c) */
	if (state->unit_index == NONE || !network_objects_client_has(state->unit_index) ||
		!object_try_and_get_and_verify_type(state->unit_index, _object_mask_unit) ||
		TEST_FLAG(object_get(state->unit_index)->object.damage_flags, _object_dead_bit))
	{
		return;
	}
	if (player->unit_index != state->unit_index)
	{
		if (player->unit_index != NONE)
			network_player_detach_unit(player_index);
		network_player_attach_unit(player_index, state->unit_index);
		/* (a unit of a life this machine missed the end of, no player's
		now: unit_kill_no_statistics is for players' units only) */
		if (unit_index != NONE && unit_index != state->unit_index)
			unit_kill(unit_index);
	}
	unit_index = state->unit_index;
	/* the seat it rides: a client's own player's, once it has ridden
	otherwise for longer than its prediction takes to reach the host and
	come back */
	{
		struct unit_datum *unit = unit_get(unit_index);
		long vehicle_index = unit->object.parent_object_index != NONE && unit->unit.parent_seat_index != NONE ?
			unit->object.parent_object_index : NONE;
		boolean same = vehicle_index == state->vehicle_index &&
			(vehicle_index == NONE || unit->unit.parent_seat_index == state->seat_index);
		short *disagreement = &distributed_seat_disagreements[state->player_index];

		if (same)
			*disagreement = 0;
		else if (!local || ++*disagreement > MAX(SEAT_DISAGREEMENT_TICKS,
			(short)ceil(distributed_own_round_trip) + SEAT_DISAGREEMENT_SLACK_TICKS))
		{
			network_objects_set_seat(unit_index, state->vehicle_index, state->seat_index);
			*disagreement = 0;
		}
	}
	{
		struct damage_network_state damage;

		damage.shield_depleted = TEST_FLAG(state->flags, _distributed_unit_shield_depleted_bit);
		damage.shield_charging = TEST_FLAG(state->flags, _distributed_unit_shield_charging_bit);
		damage.shield_over_charging = TEST_FLAG(state->flags, _distributed_unit_shield_over_charging_bit);
		damage.body_vitality = distributed_vitality_unpack(state->body_vitality);
		damage.shield_vitality = distributed_vitality_unpack(state->shield_vitality);
		damage.current_body_damage = distributed_vitality_unpack(state->current_body_damage);
		damage.recent_body_damage = distributed_vitality_unpack(state->recent_body_damage);
		damage.current_shield_damage = distributed_vitality_unpack(state->current_shield_damage);
		damage.recent_shield_damage = distributed_vitality_unpack(state->recent_shield_damage);
		damage_set_network_state(unit_index, &damage);
	}
	/* the host's powerups (the host decides pickups) */
	{
		struct unit_datum *unit = unit_get(unit_index);

		csmemcpy(player->powerup_durations, state->powerup_durations, sizeof(player->powerup_durations));
		SET_FLAG(unit->unit.flags, _unit_active_camouflaged_bit,
			TEST_FLAG(state->unit_flags, _distributed_unit_camouflaged_bit));
		SET_FLAG(unit->unit.flags, _unit_super_camouflaged_bit,
			TEST_FLAG(state->unit_flags, _distributed_unit_super_camouflaged_bit));
		unit->unit.active_camouflage = (real)state->active_camouflage / 255.0f;
	}
	if (TEST_FLAG(state->flags, _distributed_unit_placed_bit) &&
		object_get(unit_index)->object.parent_object_index == NONE)
	{
		if (local)
			distributed_correct_own_unit(player, unit_index, state);
		else
		{
			distributed_apply_state(unit_index, state, &state->position, REMOTE_CORRECTION_TOLERANCE,
				REMOTE_BLEND_DISTANCE, 0.0f, 0.0f);
		}
	}
}

/* (a client) the host's word on every player's unit */
static void distributed_handle_unit_states(
	byte const *data,
	byte const *end,
	short count)
{
	short index;

	for (index = 0; index < count; index++)
	{
		struct distributed_unit_state state;
		word size = distributed_unit_state_read(data, end, &state);

		if (!size)
			break;
		data += size;
		distributed_handle_unit_state(&state);
	}
}

/* ---------- input */

/* (a client) its own players' input of the tick just run, with the buttons
of the ticks before it, to the host */
static void distributed_client_send_inputs(
	void)
{
	struct
	{
		struct distributed_message_header header;
		struct distributed_player_input inputs[MAXIMUM_LOCAL_PLAYERS];
	} message;
	short count = 0;
	short local_player_index;

	for (local_player_index = 0; local_player_index < MAXIMUM_LOCAL_PLAYERS; local_player_index++)
	{
		long player_index = local_player_get_player_index(local_player_index);
		struct distributed_player_input *input = &message.inputs[count];

		if (player_index == NONE)
			continue;
		csmemset(input, 0, sizeof(*input));
		if (!update_client_distributed_input(local_player_index, &input->tick, &input->action, input->control_flags))
			continue;
		input->player_index = distributed_player_to_byte(player_index);
		input->host_time = distributed_host_time;
		count++;
	}
	if (count)
	{
		distributed_send(&message, _distributed_message_player_inputs, count,
			(word)(sizeof(message.header) + count * sizeof(struct distributed_player_input)), _distributed_to_host);
	}
}

/* (the host) a client's players' input, and from it (once a message) how
long a message takes that client and back */
static void distributed_handle_inputs(
	long machine_index,
	struct distributed_player_input const *inputs,
	short count)
{
	long host_time = NONE;
	short index;

	for (index = 0; index < count; index++)
	{
		struct distributed_player_input const *input = &inputs[index];
		struct player_datum *player;
		struct player_action action;

		if (input->player_index >= MAXIMUM_TRACKED_PLAYERS ||
			!distributed_machine_has_player(machine_index, input->player_index))
		{
			continue;
		}
		player = distributed_player(input->player_index);
		if (!player)
			continue;
		/* (the sticks and the trigger no further than a controller's) */
		action = input->action;
		action.throttle.i = PIN(action.throttle.i, -1.0f, 1.0f);
		action.throttle.j = PIN(action.throttle.j, -1.0f, 1.0f);
		action.primary_trigger = PIN(action.primary_trigger, 0.0f, 1.0f);
		update_server_handle_distributed_input(DATUM_INDEX_NEW(input->player_index, player->identifier), input->tick,
			&action, input->control_flags, DISTRIBUTED_INPUT_HISTORY);
		if (input->host_time != NONE && (host_time == NONE || input->host_time > host_time))
			host_time = input->host_time;
	}
	if (host_time != NONE && machine_index >= 0 && machine_index < HALO_PORT_MAXIMUM_NETWORK_MACHINES)
	{
		long sample = game_time_get() - host_time;

		/* (a longer one as the longest taken) */
		if (sample >= 0)
		{
			sample = MIN(sample, MAXIMUM_ROUND_TRIP_TICKS);
			/* (as TCP smooths its own: an eighth, a quarter) */
			if (!distributed_round_trips[machine_index].valid)
			{
				distributed_round_trips[machine_index].valid = TRUE;
				distributed_round_trips[machine_index].average = (real)sample;
				distributed_round_trips[machine_index].deviation = (real)sample / 2.0f;
			}
			else
			{
				real difference = (real)sample - distributed_round_trips[machine_index].average;

				distributed_round_trips[machine_index].average += difference / 8.0f;
				distributed_round_trips[machine_index].deviation +=
					((real)fabs(difference) - distributed_round_trips[machine_index].deviation) / 4.0f;
			}
		}
	}
}

/* the cluster the object (or what it rides) is in, NONE for none */
static short distributed_object_cluster(
	long object_index)
{
	struct object_datum *object = object_get(object_index);

	while (object->object.parent_object_index != NONE)
		object = object_get(object->object.parent_object_index);
	return object->object.location.cluster_index;
}

/* the player's input as the host's tick ran it, in fewer bytes; whether
their buttons or choices changed in the ticks it carries */
static boolean distributed_relayed_action_from(
	short player_index,
	struct player_action const **recent,
	short const *recent_counts,
	struct distributed_relayed_action *relayed)
{
	struct player_action const *action = &recent[0][player_index];
	real throttle_i = action->throttle.i > 1.0f ? 1.0f : action->throttle.i < -1.0f ? -1.0f : action->throttle.i;
	real throttle_j = action->throttle.j > 1.0f ? 1.0f : action->throttle.j < -1.0f ? -1.0f : action->throttle.j;
	real trigger = action->primary_trigger > 1.0f ? 1.0f : action->primary_trigger < 0.0f ? 0.0f :
		action->primary_trigger;
	boolean changed = FALSE;
	short history;

	csmemset(relayed, 0, sizeof(*relayed));
	relayed->player_index = (byte)player_index;
	relayed->desired_weapon_index = (signed char)action->desired_weapon_index;
	relayed->desired_grenade_index = (signed char)action->desired_grenade_index;
	relayed->desired_zoom_level = (signed char)MIN(action->desired_zoom_level, 127);
	for (history = 0; history < DISTRIBUTED_INPUT_HISTORY; history++)
	{
		struct player_action const *past = recent[history] && player_index < recent_counts[history] ?
			&recent[history][player_index] : NULL;

		relayed->control_flags[history] = past ? (unsigned short)past->control_flags : 0;
		if (history > 0 && (!past || relayed->control_flags[history] != relayed->control_flags[history - 1] ||
			past->desired_weapon_index != action->desired_weapon_index ||
			past->desired_grenade_index != action->desired_grenade_index ||
			past->desired_zoom_level != action->desired_zoom_level))
		{
			changed = TRUE;
		}
	}
	relayed->yaw = distributed_angle_pack(action->desired_facing.yaw);
	relayed->pitch = distributed_angle_pack(action->desired_facing.pitch);
	relayed->throttle_i = (signed char)(long)floor(throttle_i * 127.0f + 0.5f);
	relayed->throttle_j = (signed char)(long)floor(throttle_j * 127.0f + 0.5f);
	relayed->primary_trigger = (byte)(long)floor(trigger * 255.0f + 0.5f);
	return changed;
}

/* the action's bytes on the wire (at most
DISTRIBUTED_RELAYED_ACTION_MAXIMUM_SIZE): its player and parts, the
choices, facing, sticks and trigger, the latest tick's buttons, and those of
the ticks before only where they differ from the tick after's
(distributed_relayed_action_read) */
static word distributed_relayed_action_write(
	struct distributed_relayed_action const *relayed,
	byte *buffer)
{
	byte parts = 0;
	byte *cursor = buffer + 2;
	short history;

	cursor = distributed_put(cursor, &relayed->desired_weapon_index, sizeof(relayed->desired_weapon_index));
	cursor = distributed_put(cursor, &relayed->desired_grenade_index, sizeof(relayed->desired_grenade_index));
	cursor = distributed_put(cursor, &relayed->desired_zoom_level, sizeof(relayed->desired_zoom_level));
	cursor = distributed_put(cursor, &relayed->yaw, sizeof(relayed->yaw));
	cursor = distributed_put(cursor, &relayed->pitch, sizeof(relayed->pitch));
	cursor = distributed_put(cursor, &relayed->throttle_i, sizeof(relayed->throttle_i));
	cursor = distributed_put(cursor, &relayed->throttle_j, sizeof(relayed->throttle_j));
	cursor = distributed_put(cursor, &relayed->primary_trigger, sizeof(relayed->primary_trigger));
	cursor = distributed_put(cursor, &relayed->control_flags[0], sizeof(relayed->control_flags[0]));
	for (history = 1; history < DISTRIBUTED_INPUT_HISTORY; history++)
	{
		if (relayed->control_flags[history] != relayed->control_flags[history - 1])
		{
			SET_FLAG(parts, history - 1, TRUE);
			cursor = distributed_put(cursor, &relayed->control_flags[history], sizeof(relayed->control_flags[history]));
		}
	}
	buffer[0] = relayed->player_index;
	buffer[1] = parts;
	return (word)(cursor - buffer);
}

/* an action from its bytes on the wire, no further than end: the bytes it
took, 0 for none that make one */
static word distributed_relayed_action_read(
	byte const *buffer,
	byte const *end,
	struct distributed_relayed_action *relayed)
{
	byte const *cursor = buffer;
	byte parts;
	short history;

	csmemset(relayed, 0, sizeof(*relayed));
	if (!distributed_take(&cursor, end, &relayed->player_index, sizeof(relayed->player_index)) ||
		!distributed_take(&cursor, end, &parts, sizeof(parts)) ||
		!distributed_take(&cursor, end, &relayed->desired_weapon_index, sizeof(relayed->desired_weapon_index)) ||
		!distributed_take(&cursor, end, &relayed->desired_grenade_index, sizeof(relayed->desired_grenade_index)) ||
		!distributed_take(&cursor, end, &relayed->desired_zoom_level, sizeof(relayed->desired_zoom_level)) ||
		!distributed_take(&cursor, end, &relayed->yaw, sizeof(relayed->yaw)) ||
		!distributed_take(&cursor, end, &relayed->pitch, sizeof(relayed->pitch)) ||
		!distributed_take(&cursor, end, &relayed->throttle_i, sizeof(relayed->throttle_i)) ||
		!distributed_take(&cursor, end, &relayed->throttle_j, sizeof(relayed->throttle_j)) ||
		!distributed_take(&cursor, end, &relayed->primary_trigger, sizeof(relayed->primary_trigger)) ||
		!distributed_take(&cursor, end, &relayed->control_flags[0], sizeof(relayed->control_flags[0])))
	{
		return 0;
	}
	for (history = 1; history < DISTRIBUTED_INPUT_HISTORY; history++)
	{
		if (!TEST_FLAG(parts, history - 1))
			relayed->control_flags[history] = relayed->control_flags[history - 1];
		else if (!distributed_take(&cursor, end, &relayed->control_flags[history],
			sizeof(relayed->control_flags[history])))
		{
			return 0;
		}
	}
	return (word)(cursor - buffer);
}

/* (the host) what each client is sent this tick of every player: their unit,
and their input as its last tick ran it (with the buttons of the ticks
before it). Those near the client's own players every tick, those further,
or out of their sight, less often (NEAR_PLAYER_DISTANCE), and those dead or
not yet spawned as those out of sight; whose life, seat or shields' state
changed, or who came into sight, at once; whose buttons changed lately,
their input every tick; one who has left the game, only a change. A
client's own players' units every tick, their input never (it has its own),
and no dead player's input (it drives nothing). A client whose players are
dead watches from where they last were alive. Decided before the damage is
sent (network_damage.c), which goes where the players it hurt do; sent
after it (distributed_host_send_players). */
static void distributed_host_plan_players(
	void)
{
	static boolean present[MAXIMUM_TRACKED_PLAYERS];
	static boolean changed[MAXIMUM_TRACKED_PLAYERS];
	static boolean has_action[MAXIMUM_TRACKED_PLAYERS];
	static boolean action_changed[MAXIMUM_TRACKED_PLAYERS];
	static boolean quit[MAXIMUM_TRACKED_PLAYERS];
	static real_point3d origins[MAXIMUM_TRACKED_PLAYERS];
	static short clusters[MAXIMUM_TRACKED_PLAYERS];
	static boolean placed[MAXIMUM_TRACKED_PLAYERS];
	long machine_indices[HALO_PORT_MAXIMUM_NETWORK_MACHINES];
	short machine_count = distributed_client_machines(machine_indices, HALO_PORT_MAXIMUM_NETWORK_MACHINES);
	struct structure_bsp *structure_bsp = global_structure_bsp_get();
	short cluster_count = structure_bsp ? (short)structure_bsp->clusters.count : 0;
	long now = game_time_get();
	struct player_action const *recent[DISTRIBUTED_INPUT_HISTORY];
	short recent_counts[DISTRIBUTED_INPUT_HISTORY];
	short player_index;
	short machine_number;
	short history;

	distributed_host_update_number = update_server_ticked_update_number();
	for (history = 0; history < DISTRIBUTED_INPUT_HISTORY; history++)
	{
		recent[history] = distributed_host_update_number != NONE ?
			update_server_update_actions(distributed_host_update_number - history, &recent_counts[history]) : NULL;
	}
	csmemset(distributed_sends, 0, sizeof(distributed_sends));
	/* (the client each player is on) */
	for (player_index = 0; player_index < MAXIMUM_TRACKED_PLAYERS; player_index++)
		distributed_player_machines[player_index] = NONE;
	for (machine_number = 0; machine_number < machine_count; machine_number++)
	{
		long *player_list = machine_get_player_list(machine_indices[machine_number]);
		short local_player_index;

		for (local_player_index = 0; local_player_index < MAXIMUM_LOCAL_PLAYERS; local_player_index++)
		{
			short index = player_list[local_player_index] != NONE ?
				(short)DATUM_INDEX_TO_ABSOLUTE_INDEX(player_list[local_player_index]) : NONE;

			if (index >= 0 && index < MAXIMUM_TRACKED_PLAYERS)
				distributed_player_machines[index] = machine_indices[machine_number];
		}
	}
	for (player_index = 0; player_index < MAXIMUM_TRACKED_PLAYERS; player_index++)
	{
		struct player_datum *player = distributed_player(player_index);
		struct distributed_unit_state *state = &distributed_host_states[player_index];
		long unit_index;

		present[player_index] = player != NULL;
		placed[player_index] = FALSE;
		has_action[player_index] = FALSE;
		if (!player)
			continue;
		quit[player_index] = player->quit_out_of_game;
		distributed_state_from_player(player_index, state);
		unit_index = distributed_living_unit(player);
		if (unit_index != NONE)
		{
			object_get_origin(unit_index, &origins[player_index]);
			clusters[player_index] = distributed_object_cluster(unit_index);
			placed[player_index] = TRUE;
		}
		changed[player_index] =
			/* (the predicted bit is its own client's alone) */
			distributed_sent_units[player_index].flags != (byte)(state->flags & ~FLAG(_distributed_unit_predicted_bit)) ||
			distributed_sent_units[player_index].unit_index != state->unit_index ||
			distributed_sent_units[player_index].vehicle_index != state->vehicle_index ||
			distributed_sent_units[player_index].seat_index != state->seat_index;
		distributed_sent_units[player_index].flags = (byte)(state->flags & ~FLAG(_distributed_unit_predicted_bit));
		distributed_sent_units[player_index].unit_index = state->unit_index;
		distributed_sent_units[player_index].vehicle_index = state->vehicle_index;
		distributed_sent_units[player_index].seat_index = state->seat_index;
		if (placed[player_index] && recent[0] && player_index < recent_counts[0])
		{
			has_action[player_index] = TRUE;
			action_changed[player_index] = distributed_relayed_action_from(player_index, recent, recent_counts,
				&distributed_host_actions[player_index]);
		}
	}
	for (machine_number = 0; machine_number < machine_count; machine_number++)
	{
		long machine_index = machine_indices[machine_number];
		long *player_list = machine_get_player_list(machine_index);
		real_point3d viewers[MAXIMUM_LOCAL_PLAYERS];
		unsigned long *viewer_pvs[MAXIMUM_LOCAL_PLAYERS];
		real_vector3d viewer_aims[MAXIMUM_LOCAL_PLAYERS];
		boolean viewer_scoped[MAXIMUM_LOCAL_PLAYERS];
		short viewer_count = 0;
		short index;

		/* its players where they are, or were when last alive (each viewer's
		row of the map's potentially visible set) */
		for (index = 0; index < MAXIMUM_LOCAL_PLAYERS; index++)
		{
			short viewer_index = player_list[index] != NONE ?
				(short)DATUM_INDEX_TO_ABSOLUTE_INDEX(player_list[index]) : NONE;
			struct distributed_viewer *viewer = &distributed_viewers[machine_index][index];
			boolean scoped = FALSE;

			if (viewer_index < 0 || viewer_index >= MAXIMUM_TRACKED_PLAYERS)
				viewer->valid = FALSE;
			else if (placed[viewer_index])
			{
				struct unit_datum *unit = unit_get(distributed_living_unit(distributed_player(viewer_index)));

				viewer->valid = TRUE;
				viewer->cluster_index = clusters[viewer_index];
				viewer->position = origins[viewer_index];
				viewer->aim = unit->unit.aiming_vector;
				/* (NONE unzoomed: a char, which is unsigned on ARM) */
				scoped = (signed char)unit->unit.current_zoom_level >= 0;
			}
			if (!viewer->valid)
				continue;
			viewers[viewer_count] = viewer->position;
			viewer_pvs[viewer_count] = viewer->cluster_index >= 0 && viewer->cluster_index < cluster_count ?
				structure_bsp_get_cluster_pvs(structure_bsp, viewer->cluster_index) : NULL;
			viewer_aims[viewer_count] = viewer->aim;
			viewer_scoped[viewer_count++] = scoped;
		}
		for (player_index = 0; player_index < MAXIMUM_TRACKED_PLAYERS; player_index++)
		{
			boolean own = distributed_player_machines[player_index] == machine_index;
			byte *sends = &distributed_sends[machine_index][player_index];
			boolean send = FALSE;
			boolean seen = TRUE;
			short period = 1;

			if (!present[player_index] || (quit[player_index] && !own && !changed[player_index]))
				continue;
			if (!own && !placed[player_index])
			{
				/* (dead, or not yet spawned: a spawn is a change, sent at once) */
				period = HIDDEN_PLAYER_PERIOD_TICKS;
				distributed_seen[machine_index][player_index] = TRUE;
			}
			/* (a client that has had no player alive watches anyone) */
			else if (!own && viewer_count)
			{
				real distances_squared[MAXIMUM_LOCAL_PLAYERS];
				boolean sees[MAXIMUM_LOCAL_PLAYERS];
				short to = clusters[player_index];
				real nearest = -1.0f;
				boolean visible = FALSE;

				for (index = 0; index < viewer_count; index++)
				{
					real dx = origins[player_index].x - viewers[index].x;
					real dy = origins[player_index].y - viewers[index].y;
					real dz = origins[player_index].z - viewers[index].z;

					distances_squared[index] = dx * dx + dy * dy + dz * dz;
					if (nearest < 0.0f || distances_squared[index] < nearest)
						nearest = distances_squared[index];
					/* (outside the map's clusters: in sight, to be safe) */
					sees[index] = !viewer_pvs[index] || to < 0 || to >= cluster_count ||
						BIT_VECTOR_TEST_FLAG(viewer_pvs[index], to);
					visible |= sees[index];
				}
				period = !visible ? HIDDEN_PLAYER_PERIOD_TICKS :
					nearest < NEAR_PLAYER_DISTANCE * NEAR_PLAYER_DISTANCE ? 1 :
					nearest < MIDDLE_PLAYER_DISTANCE * MIDDLE_PLAYER_DISTANCE ? 2 :
					nearest < FAR_PLAYER_DISTANCE * FAR_PLAYER_DISTANCE ? 3 : 4;
				/* (came into sight: at once) */
				if (visible && !distributed_seen[machine_index][player_index])
					send = TRUE;
				distributed_seen[machine_index][player_index] = visible;
				seen = visible;
				/* aimed near, when not sent this tick otherwise: the cosine of the
				angle off the aim, compared squared (along / distance >= cosine) */
				if (!send && !changed[player_index] && period > 1 && (now + player_index) % period != 0)
				{
					short aimed_period = HIDDEN_PLAYER_PERIOD_TICKS;

					for (index = 0; index < viewer_count; index++)
					{
						real along;

						if (!sees[index])
							continue;
						along = (origins[player_index].x - viewers[index].x) * viewer_aims[index].i +
							(origins[player_index].y - viewers[index].y) * viewer_aims[index].j +
							(origins[player_index].z - viewers[index].z) * viewer_aims[index].k;
						if (along > 0.0f && viewer_scoped[index] &&
							along * along >= SCOPED_AT_COSINE * SCOPED_AT_COSINE * distances_squared[index])
						{
							aimed_period = 1;
						}
						else if (along > 0.0f &&
							along * along >= AIMED_AT_COSINE * AIMED_AT_COSINE * distances_squared[index])
						{
							aimed_period = MIN(aimed_period, 2);
						}
					}
					period = MIN(period, aimed_period);
				}
			}
			else
			{
				distributed_seen[machine_index][player_index] = TRUE;
			}
			send |= changed[player_index] || (now + player_index) % period == 0;
			if (seen || own)
				SET_FLAG(*sends, _distributed_send_sees_bit, TRUE);
			if (send || own)
				SET_FLAG(*sends, _distributed_send_state_bit, TRUE);
			if (!own && has_action[player_index] && (send || action_changed[player_index]))
				SET_FLAG(*sends, _distributed_send_action_bit, TRUE);
		}
	}
}

/* (the host) to each client what distributed_host_plan_players decided, the
input before the units, as the host's tick had them */
static void distributed_host_send_players(
	void)
{
	long machine_indices[HALO_PORT_MAXIMUM_NETWORK_MACHINES];
	short machine_count = distributed_client_machines(machine_indices, HALO_PORT_MAXIMUM_NETWORK_MACHINES);
	short machine_number;

	for (machine_number = 0; machine_number < machine_count; machine_number++)
	{
		long machine_index = machine_indices[machine_number];
		short player_index;

		distributed_packer_begin(&distributed_packer, (short)machine_index, _distributed_message_relayed_actions,
			&distributed_host_update_number, sizeof(distributed_host_update_number));
		for (player_index = 0; player_index < MAXIMUM_TRACKED_PLAYERS; player_index++)
		{
			byte entry[DISTRIBUTED_RELAYED_ACTION_MAXIMUM_SIZE];

			if (TEST_FLAG(distributed_sends[machine_index][player_index], _distributed_send_action_bit))
			{
				distributed_packer_add(&distributed_packer, entry,
					distributed_relayed_action_write(&distributed_host_actions[player_index], entry));
			}
		}
		distributed_packer_flush(&distributed_packer);
		distributed_packer_begin(&distributed_packer, (short)machine_index, _distributed_message_unit_states, NULL, 0);
		for (player_index = 0; player_index < MAXIMUM_TRACKED_PLAYERS; player_index++)
		{
			byte entry[DISTRIBUTED_UNIT_STATE_MAXIMUM_SIZE];

			if (TEST_FLAG(distributed_sends[machine_index][player_index], _distributed_send_state_bit))
			{
				distributed_packer_add(&distributed_packer, entry,
					distributed_unit_state_write(&distributed_host_states[player_index],
						distributed_player_machines[player_index] == machine_index, entry));
			}
		}
		distributed_packer_flush(&distributed_packer);
	}
}

/* (a client) the host's input for its players, for the others it drives:
the host's update, then each player's */
static void distributed_handle_actions(
	byte const *data,
	byte const *end,
	short count)
{
	long update_number;
	short index;

	if (!distributed_take(&data, end, &update_number, sizeof(update_number)))
		return;
	for (index = 0; index < count; index++)
	{
		struct distributed_relayed_action relayed;
		struct player_action action;
		word size = distributed_relayed_action_read(data, end, &relayed);

		if (!size)
			break;
		data += size;
		csmemset(&action, 0, sizeof(action));
		action.control_flags = relayed.control_flags[0];
		action.desired_facing.yaw = distributed_angle_unpack(relayed.yaw, FALSE);
		action.desired_facing.pitch = distributed_angle_unpack(relayed.pitch, TRUE);
		action.throttle.i = (real)relayed.throttle_i / 127.0f;
		action.throttle.j = (real)relayed.throttle_j / 127.0f;
		action.primary_trigger = (real)relayed.primary_trigger / 255.0f;
		action.desired_weapon_index = relayed.desired_weapon_index;
		action.desired_grenade_index = relayed.desired_grenade_index;
		action.desired_zoom_level = relayed.desired_zoom_level;
		update_client_handle_relayed_action(relayed.player_index, update_number, &action,
			relayed.control_flags, DISTRIBUTED_INPUT_HISTORY);
	}
}

/* ---------- deaths */

/* a player died (game_engine_player_killed, before it announces who killed
whom): the host notes who killed them for its clients; on a client, whose
copy of the death knows nothing of the killer, the host's killer, as this
machine has them */
void network_distributed_player_killed(
	long *killing_player_index,
	long *killing_object_index,
	long dead_player_index,
	boolean *friendly_fire)
{
	short dead_absolute_index = (short)DATUM_INDEX_TO_ABSOLUTE_INDEX(dead_player_index);
	struct distributed_death *death;

	if (dead_absolute_index < 0 || dead_absolute_index >= MAXIMUM_TRACKED_PLAYERS)
		return;
	death = &distributed_deaths[dead_absolute_index];
	if (game_connection() == _game_connection_network_server)
	{
		struct object_datum *killing_object = *killing_object_index != NONE ?
			object_try_and_get(*killing_object_index) : NULL;

		death->valid = TRUE;
		death->killing_player_index = *killing_player_index != NONE ?
			(short)DATUM_INDEX_TO_ABSOLUTE_INDEX(*killing_player_index) : NONE;
		death->friendly_fire = *friendly_fire;
		death->killed_by_vehicle = *killing_player_index == NONE && killing_object &&
			killing_object->object.type == _object_type_vehicle;
		distributed_statistics_due = TRUE;
	}
	else if (game_connection() == _game_connection_network_client && death->valid)
	{
		struct player_datum *killing_player = death->killing_player_index != NONE ?
			distributed_player(death->killing_player_index) : NULL;

		*friendly_fire = death->friendly_fire;
		*killing_player_index = NONE;
		if (killing_player)
		{
			*killing_player_index = DATUM_INDEX_NEW(death->killing_player_index, killing_player->identifier);
			*killing_object_index = killing_player->unit_index;
		}
		/* (an empty vehicle's: the one that did it, the same object here) */
		else if (!death->killed_by_vehicle || *killing_object_index == NONE ||
			!object_try_and_get(*killing_object_index) ||
			object_get(*killing_object_index)->object.type != _object_type_vehicle)
		{
			*killing_object_index = NONE;
		}
	}
}

/* (a client) the host's word on how a player died, with the damage that
killed them (network_damage.c), before this machine's copy dies */
void distributed_set_death(
	short dead_player_index,
	byte killing_player_index,
	boolean friendly_fire,
	boolean killed_by_vehicle)
{
	struct distributed_death *death;

	if (dead_player_index < 0 || dead_player_index >= MAXIMUM_TRACKED_PLAYERS)
		return;
	death = &distributed_deaths[dead_player_index];
	death->valid = TRUE;
	death->killing_player_index = killing_player_index != NO_PLAYER ? killing_player_index : NONE;
	death->friendly_fire = friendly_fire;
	death->killed_by_vehicle = killed_by_vehicle;
}

/* (the host) how a player died, for the damage that killed them */
boolean distributed_get_death(
	short dead_player_index,
	byte *killing_player_index,
	boolean *friendly_fire,
	boolean *killed_by_vehicle)
{
	struct distributed_death const *death;

	if (dead_player_index < 0 || dead_player_index >= MAXIMUM_TRACKED_PLAYERS)
		return FALSE;
	death = &distributed_deaths[dead_player_index];
	*killing_player_index = death->killing_player_index != NONE ? (byte)death->killing_player_index : NO_PLAYER;
	*friendly_fire = death->friendly_fire;
	*killed_by_vehicle = death->killed_by_vehicle;
	return death->valid;
}

/* ---------- pickups */

/* (the host) a player on another machine picked something up (players.c),
for that machine to show */
void network_distributed_player_picked_up(
	long player_index,
	short kind,
	long definition_index,
	short count)
{
	struct distributed_pickup *pickup;

	if (game_connection() != _game_connection_network_server ||
		distributed_pickup_count >= MAXIMUM_PICKUPS_PER_TICK)
	{
		return;
	}
	pickup = &distributed_pickups[distributed_pickup_count++];
	pickup->player_index = distributed_player_to_byte(player_index);
	pickup->kind = (byte)kind;
	pickup->count = count;
	pickup->definition_index = definition_index;
}

/* (the host) each machine what its players picked up (no other machine
shows it) */
static void distributed_send_pickups(
	void)
{
	struct
	{
		struct distributed_message_header header;
		struct distributed_pickup pickups[MAXIMUM_PICKUPS_PER_TICK];
	} message;
	short index;

	for (index = 0; index < distributed_pickup_count; index++)
	{
		long machine_index = distributed_player_machine(distributed_pickups[index].player_index);
		short other;
		short count = 0;

		/* (each machine once, with all of its players') */
		for (other = 0; other < index; other++)
		{
			if (distributed_player_machine(distributed_pickups[other].player_index) == machine_index)
				break;
		}
		if (machine_index == NONE || other < index)
			continue;
		for (other = index; other < distributed_pickup_count; other++)
		{
			if (distributed_player_machine(distributed_pickups[other].player_index) == machine_index)
				message.pickups[count++] = distributed_pickups[other];
		}
		distributed_send_to_machine_reliably(machine_index, &message, _distributed_message_pickups, count,
			(word)(sizeof(message.header) + count * sizeof(struct distributed_pickup)));
	}
	distributed_pickup_count = 0;
}

/* ---------- statistics */

/* the bytes' checksum (FNV-1a) */
static unsigned long distributed_checksum(
	void const *data,
	long size)
{
	byte const *bytes = (byte const *)data;
	unsigned long checksum = 2166136261UL;
	long index;

	for (index = 0; index < size; index++)
		checksum = (checksum ^ bytes[index]) * 16777619UL;
	return checksum;
}

/* (the host) every player's ping (distributed_player_ping), for the
clients' scoreboards */
static void distributed_send_pings(
	void)
{
	struct distributed_pings_message message;
	short limit = MIN(MAXIMUM_PINGS_PER_MESSAGE, RELIABLE_ENTRIES(struct distributed_player_ping));
	short count = 0;
	short player_index;

	for (player_index = 0; player_index < MAXIMUM_TRACKED_PLAYERS; player_index++)
	{
		long ping = distributed_player_ping(player_index);

		if (!distributed_player(player_index))
			continue;
		message.players[count].player_index = (byte)player_index;
		message.players[count].pad = 0;
		message.players[count].milliseconds = ping == NONE ? UNKNOWN_PING : (word)MIN(ping, UNKNOWN_PING - 1);
		count++;
		if (count == limit)
		{
			distributed_send(&message, _distributed_message_pings, count,
				(word)(sizeof(message.header) + count * sizeof(struct distributed_player_ping)),
				_distributed_to_clients);
			count = 0;
		}
	}
	if (count)
	{
		distributed_send(&message, _distributed_message_pings, count,
			(word)(sizeof(message.header) + count * sizeof(struct distributed_player_ping)),
			_distributed_to_clients);
	}
}

/* the players' statistics that changed since they were last sent, and when
refreshing, STATISTICS_REFRESH_PLAYERS more whatever they are, round them
all (a client that lost a change has it again within eight seconds) */
static void distributed_send_statistics(
	boolean refresh)
{
	struct distributed_statistics_message message;
	short limit = MIN(MAXIMUM_STATISTICS_PER_MESSAGE, RELIABLE_ENTRIES(struct distributed_player_statistics));
	short count = 0;
	short refreshed = refresh ? 0 : STATISTICS_REFRESH_PLAYERS;
	/* (where this round starts: the cursor moves past the last player it
	refreshed, which must not shift the players this round visits) */
	short first = distributed_statistics_cursor;
	short step;

	for (step = 0; step < MAXIMUM_TRACKED_PLAYERS; step++)
	{
		short player_index = (short)((first + step) % MAXIMUM_TRACKED_PLAYERS);
		struct player_datum *player = distributed_player(player_index);
		unsigned long checksum;

		if (!player)
			continue;
		checksum = distributed_checksum(&player->statistics, sizeof(player->statistics));
		if (checksum == distributed_sent_statistics[player_index])
		{
			if (refreshed >= STATISTICS_REFRESH_PLAYERS)
				continue;
			refreshed++;
			distributed_statistics_cursor = (short)((player_index + 1) % MAXIMUM_TRACKED_PLAYERS);
		}
		distributed_sent_statistics[player_index] = checksum;
		message.players[count].player_index = player_index;
		message.players[count].pad = 0;
		message.players[count].statistics = player->statistics;
		count++;
		if (count == limit)
		{
			distributed_send(&message, _distributed_message_player_statistics, count,
				(word)(sizeof(message.header) + count * sizeof(struct distributed_player_statistics)),
				_distributed_to_clients);
			count = 0;
		}
	}
	if (count)
	{
		distributed_send(&message, _distributed_message_player_statistics, count,
			(word)(sizeof(message.header) + count * sizeof(struct distributed_player_statistics)),
			_distributed_to_clients);
	}
}

/* every player's statistics, to a machine that has loaded (the others', and
what was last sent them, as they are) */
static void distributed_send_all_statistics(
	long machine_index)
{
	struct distributed_statistics_message message;
	short limit = MIN(MAXIMUM_STATISTICS_PER_MESSAGE, RELIABLE_ENTRIES(struct distributed_player_statistics));
	short count = 0;
	short player_index;

	for (player_index = 0; player_index < MAXIMUM_TRACKED_PLAYERS; player_index++)
	{
		struct player_datum *player = distributed_player(player_index);

		if (!player)
			continue;
		message.players[count].player_index = player_index;
		message.players[count].pad = 0;
		message.players[count].statistics = player->statistics;
		count++;
		if (count == limit)
		{
			distributed_send_to_machine(machine_index, &message, _distributed_message_player_statistics, count,
				(word)(sizeof(message.header) + count * sizeof(struct distributed_player_statistics)));
			count = 0;
		}
	}
	if (count)
	{
		distributed_send_to_machine(machine_index, &message, _distributed_message_player_statistics, count,
			(word)(sizeof(message.header) + count * sizeof(struct distributed_player_statistics)));
	}
}

/* ---------- the game type's state */

/* the game type's state (larger than a datagram: reliably) to a machine
that has loaded, or (NONE) to every client when it changed (at most every
other look, unless the game ended) */
static void distributed_send_game_state(
	long machine_index)
{
	struct
	{
		struct distributed_message_header header;
		byte data[MAXIMUM_GAME_STATE_SIZE];
	} message;
	long size = game_engine_write_network_state(message.data, sizeof(message.data));
	unsigned long checksum;

	if (size <= 0)
		return;
	if (machine_index != NONE)
	{
		distributed_send_to_machine_reliably(machine_index, &message, _distributed_message_game_state, 0,
			(word)(sizeof(message.header) + size));
		return;
	}
	checksum = distributed_checksum(message.data, size);
	if (distributed_game_state_time != NONE &&
		(checksum == distributed_game_state_checksum ||
			(game_time_get() - distributed_game_state_time < GAME_STATE_MINIMUM_TICKS &&
				(size < (long)sizeof(long) ||
					csmemcmp(message.data, &distributed_game_state_postgame, sizeof(long)) == 0))))
	{
		return;
	}
	if (size >= (long)sizeof(long))
		csmemcpy(&distributed_game_state_postgame, message.data, sizeof(long));
	distributed_game_state_checksum = checksum;
	distributed_game_state_time = game_time_get();
	distributed_send(&message, _distributed_message_game_state, 0, (word)(sizeof(message.header) + size),
		_distributed_to_clients_reliably);
}

/* ---------- the game */

/* a new map loading (game.c), before any of the new game's messages can
apply: nothing sent or had yet */
void network_distributed_new_game(
	void)
{
	short sender;
	short type;
	short player_index;

	distributed_last_sent_time = NONE;
	csmemset(distributed_deaths, 0, sizeof(distributed_deaths));
	csmemset(distributed_seat_disagreements, 0, sizeof(distributed_seat_disagreements));
	csmemset(distributed_predictions, 0, sizeof(distributed_predictions));
	csmemset(distributed_accepted, 0, sizeof(distributed_accepted));
	csmemset(distributed_host_speeds, 0, sizeof(distributed_host_speeds));
	csmemset(distributed_round_trips, 0, sizeof(distributed_round_trips));
	csmemset(distributed_client_clocks, 0, sizeof(distributed_client_clocks));
	csmemset(distributed_client_identities, 0, sizeof(distributed_client_identities));
	distributed_identity_sent = FALSE;
	{
		short machine_index;

		for (machine_index = 0; machine_index < HALO_PORT_MAXIMUM_NETWORK_MACHINES; machine_index++)
			distributed_client_clocks[machine_index].window_tick = NONE;
	}
	csmemset(distributed_seen, 0, sizeof(distributed_seen));
	csmemset(distributed_viewers, 0, sizeof(distributed_viewers));
	csmemset(distributed_sends, 0, sizeof(distributed_sends));
	/* (none sent: every player's the first time) */
	csmemset(distributed_sent_statistics, 0, sizeof(distributed_sent_statistics));
	distributed_statistics_cursor = 0;
	distributed_game_state_checksum = 0;
	distributed_game_state_postgame = 0;
	distributed_game_state_time = NONE;
	distributed_host_update_number = NONE;
	for (player_index = 0; player_index < MAXIMUM_TRACKED_PLAYERS; player_index++)
	{
		distributed_sent_units[player_index].flags = 0;
		distributed_sent_units[player_index].unit_index = NONE;
		distributed_sent_units[player_index].vehicle_index = NONE;
		distributed_sent_units[player_index].seat_index = NONE;
	}
	for (sender = 0; sender < MAXIMUM_SENDERS; sender++)
	{
		distributed_batches[sender].size = 0;
		for (type = 0; type < NUMBER_OF_DISTRIBUTED_MESSAGES; type++)
			distributed_received_times[sender][type] = NONE;
	}
	for (player_index = 0; player_index < MAXIMUM_TRACKED_PLAYERS; player_index++)
	{
		distributed_predictions[player_index].taken_host_time = NONE;
		distributed_host_speeds[player_index].unit_index = NONE;
		distributed_player_machines[player_index] = NONE;
	}
	for (sender = 0; sender < MAXIMUM_LOCAL_PLAYERS; sender++)
	{
		for (type = 0; type < OWN_POSITION_TICKS; type++)
		{
			distributed_own_positions[sender][type].time = NONE;
			distributed_own_positions[sender][type].unit_index = NONE;
		}
	}
	distributed_own_round_trip = 0.0f;
	csmemset(distributed_player_pings, 0xFF, sizeof(distributed_player_pings));
	csmemset(distributed_machine_players, 0xFF, sizeof(distributed_machine_players));
	distributed_machines.in_tick = FALSE;
	distributed_machines.valid = FALSE;
	distributed_host_time = NONE;
	distributed_statistics_due = FALSE;
	distributed_pickup_count = 0;
	/* (each player's latest input: player_queues_new.c) */
	update_queues_distributed_reset();
	network_objects_new_game();
	network_damage_new_game();
}

/* after each tick (game_time.c) */
void network_distributed_tick(
	void)
{
	short connection = game_connection();

	if (game_time_get() == distributed_last_sent_time)
		return;
	distributed_last_sent_time = game_time_get();
	distributed_machines.in_tick = TRUE;
	if (connection == _game_connection_network_server)
	{
		short player_index;

		/* (a living player's last death is behind them: a death the game
		does not note, one it does not score, has no killer) */
		for (player_index = 0; player_index < MAXIMUM_TRACKED_PLAYERS; player_index++)
		{
			if (distributed_deaths[player_index].valid &&
				distributed_living_unit(distributed_player(player_index)) != NONE)
			{
				distributed_deaths[player_index].valid = FALSE;
			}
		}
		/* the clients' players where they say they are, then the objects
		first (created before anything names them), the damage dealt this
		tick before the units it hurt and killed (but where the units go
		decided first: the damage goes where the players it hurt do), and a
		kill's statistics before the kill, so that a client announcing it
		counts it (a double kill, a killing spree) */
		distributed_apply_predictions();
		network_objects_apply_vehicle_predictions();
		network_objects_host_tick();
		distributed_host_plan_players();
		network_damage_host_tick();
		if (distributed_statistics_due || game_time_get() % STATISTICS_INTERVAL_TICKS == 0)
			distributed_send_statistics(game_time_get() % STATISTICS_REFRESH_TICKS == 0);
		distributed_statistics_due = FALSE;
		if (game_time_get() % PING_INTERVAL_TICKS == 0)
			distributed_send_pings();
		distributed_host_send_players();
		distributed_send_pickups();
		if (game_time_get() % GAME_STATE_INTERVAL_TICKS == 0)
			distributed_send_game_state(NONE);
	}
	else if (connection == _game_connection_network_client)
	{
		distributed_note_own_positions();
		distributed_client_send_inputs();
		distributed_client_send_predictions();
		network_objects_client_tick();
		network_damage_client_tick();
	}
	distributed_batches_flush();
	distributed_machines.in_tick = FALSE;
	distributed_machines.valid = FALSE;
}

/* whether an unreliable message of the kind is older than one had already
(then it is dropped: the newer has overtaken it) */
static boolean distributed_message_stale(
	long machine_index,
	struct distributed_message_header const *header)
{
	short sender = machine_index == NONE ? HOST_SENDER : (short)machine_index;
	long *latest;

	switch (header->type)
	{
	case _distributed_message_player_prediction:
	case _distributed_message_unit_states:
	case _distributed_message_player_statistics:
	case _distributed_message_inventories:
	case _distributed_message_object_states:
	case _distributed_message_vehicle_prediction:
	case _distributed_message_player_inputs:
	case _distributed_message_relayed_actions:
	case _distributed_message_damage_events:
	case _distributed_message_pings:
		break;
	default:
		return FALSE;
	}
	if (sender < 0 || sender >= MAXIMUM_SENDERS)
		return TRUE;
	latest = &distributed_received_times[sender][header->type];
	/* (a tick's several messages of a kind have its time alike) */
	if (*latest != NONE && header->game_time < *latest)
		return TRUE;
	*latest = header->game_time;
	return FALSE;
}

/* (the host) a text shown in red on every machine's console: its own, and
every client's (_distributed_message_notice) */
static void distributed_send_notice(
	char const *text)
{
	struct
	{
		struct distributed_message_header header;
		char text[MAXIMUM_NOTICE_LENGTH];
	} message;
	long length = csstrlen(text);

	if (length > MAXIMUM_NOTICE_LENGTH - 1)
		length = MAXIMUM_NOTICE_LENGTH - 1;
	csmemset(&message, 0, sizeof(message));
	csmemcpy(message.text, text, length);
	console_warning("%s", message.text);
	error(_error_log, "%s", message.text);
	distributed_send(&message, _distributed_message_notice, 0, (word)(sizeof(message.header) + length + 1),
		_distributed_to_clients_reliably);
}

/* (the host) the names of a client machine's players, in ASCII, for a
notice ("?" for what is not ASCII) */
static void distributed_machine_player_names(
	long machine_index,
	char *names,
	long size)
{
	struct data_iterator iterator;
	struct player_datum *player;
	long length = 0;

	names[0] = 0;
	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		short index;

		if (distributed_player_machine((short)DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.datum_index)) != machine_index)
			continue;
		if (length && length + 2 < size)
		{
			names[length++] = ',';
			names[length++] = ' ';
		}
		for (index = 0; index < (short)NUMBEROF(player->name) && player->name[index] && length + 1 < size; index++)
			names[length++] = player->name[index] >= 32 && player->name[index] < 127 ? (char)player->name[index] : '?';
		names[length] = 0;
	}
	if (!length)
		snprintf(names, size, "machine #%ld", machine_index);
}

/* (a client) its Discord user, as its Discord told it (none without one:
not running, or internet play off), to the host: once it is a machine the
host takes messages of (its ready sent: network_objects_client_tick), and
again when it changes (Discord connecting later), looked at once a second */
void distributed_client_send_identity(
	void)
{
	struct
	{
		struct distributed_message_header header;
		struct distributed_client_identity identity;
	} message;

	if (distributed_identity_sent && game_time_get() % TICKS_PER_SECOND != 0)
		return;
	csmemset(&message, 0, sizeof(message));
	p2p_discord_identity(message.identity.discord_id, sizeof(message.identity.discord_id),
		message.identity.discord_name, sizeof(message.identity.discord_name));
	if (distributed_identity_sent &&
		!csmemcmp(&message.identity, &distributed_sent_identity, sizeof(distributed_sent_identity)))
	{
		return;
	}
	distributed_sent_identity = message.identity;
	distributed_identity_sent = TRUE;
	distributed_send(&message, _distributed_message_client_identity, 1, (word)sizeof(message),
		_distributed_to_host_reliably);
}

/* (the host) a client machine's address as text: its real one, for an
internet play peer's stand-in (p2p.c) */
static void distributed_address_text(
	unsigned long address,
	char *text,
	long size)
{
	/* 100.64.0.0/10: an internet play peer's, by its real address (network
	byte order: its first number the lowest byte) */
	if ((address & 0xFFC00000) == 0x64400000)
	{
		unsigned long network = (address >> 24) | ((address >> 8) & 0xFF00) | ((address << 8) & 0xFF0000) |
			(address << 24);
		unsigned long real = p2p_peer_endpoint_address(network);

		if (real)
		{
			snprintf(text, size, "%lu.%lu.%lu.%lu", real & 255, (real >> 8) & 255, (real >> 16) & 255,
				(real >> 24) & 255);
			return;
		}
	}
	if (address)
	{
		snprintf(text, size, "%lu.%lu.%lu.%lu", (address >> 24) & 255, (address >> 16) & 255, (address >> 8) & 255,
			address & 255);
	}
	else
	{
		snprintf(text, size, "unknown");
	}
}

/* (the host) a client machine's address as text (distributed_address_text) */
static void distributed_machine_address_text(
	long machine_index,
	char *text,
	long size)
{
	distributed_address_text(network_game_server_machine_address(machine_index), text, size);
}

/* text of a player's (their names) kept to printable ASCII, no longer
than the size: no line of theirs breaks or runs on */
static void distributed_printable(
	char *destination,
	long size,
	char const *source)
{
	long length = 0;

	for (; source && *source && length < size - 1; source++)
		destination[length++] = *source >= 32 && *source < 127 ? *source : '?';
	destination[length] = 0;
}

/* (the host) a line in a list of players (CHEATERS_FILE, BANS_FILE): when,
their address, Discord user and names, and why; separated by tabs, each
part kept to the characters allowed and their lengths (what a player could
tell: their Discord user and names). The Discord user of the machine at
the index, if it is one in the game (NONE: none) */
static void distributed_write_player_record(
	char const *file_name,
	char const *address,
	long machine_index,
	char const *names,
	char const *reason)
{
	char discord_id[DISCORD_ID_SIZE] = "";
	char discord_name[DISCORD_NAME_SIZE] = "";
	char hardware_id[40] = "";
	char kept_names[96];
	char kept_reason[96];
	char kept_address[32];
	char when[32] = "";
	time_t now = time(NULL);
	struct tm *local = localtime(&now);
	FILE *file;

	if (machine_index >= 0 && machine_index < HALO_PORT_MAXIMUM_NETWORK_MACHINES && game_in_progress())
	{
		p2p_discord_sanitize(discord_id, sizeof(discord_id), distributed_client_identities[machine_index].discord_id, 0);
		p2p_discord_sanitize(discord_name, sizeof(discord_name),
			distributed_client_identities[machine_index].discord_name, 1);
	}
	if (machine_index >= 0 && machine_index < HALO_PORT_MAXIMUM_NETWORK_MACHINES)
		p2p_hardware_id_sanitize(hardware_id, 33, network_game_server_machine_hardware_id(machine_index));
	distributed_printable(kept_names, sizeof(kept_names), names);
	distributed_printable(kept_reason, sizeof(kept_reason), reason);
	distributed_printable(kept_address, sizeof(kept_address), address);
	if (local)
		strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", local);
	file = fopen(file_name, "a");
	if (!file)
	{
		error(_error_log, "could not open %s to add a player to it", file_name);
		return;
	}
	fprintf(file, "%s\tip=%s\thwid=%s\tdiscord_username=%s\tdiscord_id=%s\tplayers=%s\treason=%s\n", when,
		kept_address, hardware_id[0] ? hardware_id : "none", discord_name[0] ? discord_name : "none",
		discord_id[0] ? discord_id : "none", kept_names, kept_reason);
	fclose(file);
}

/* (the host) a player dropped for cheating: in CHEATERS_FILE, and banned
(BANS_FILE) */
static void distributed_log_cheater(
	long machine_index,
	char const *names,
	char const *reason)
{
	char address[32];

	distributed_machine_address_text(machine_index, address, sizeof(address));
	distributed_write_player_record(CHEATERS_FILE, address, machine_index, names, reason);
	distributed_write_player_record(BANS_FILE, address, machine_index, names, reason);
}

/* (the host: network_server_manager.c) whether a machine of this address
(host byte order; an internet play peer's by its real one) is banned: its
address one of BANS_FILE's (each line's "ip=", which a host may add or
take out by hand) */
boolean network_distributed_banned(
	unsigned long address,
	char const *hardware_id)
{
	char text[32];
	char line[512];
	char kept_hardware_id[40];
	FILE *file;
	boolean banned = FALSE;

	p2p_hardware_id_sanitize(kept_hardware_id, 33, hardware_id);
	if (address)
		distributed_address_text(address, text, sizeof(text));
	else
		text[0] = 0;
	file = fopen(BANS_FILE, "r");
	if (!file)
		return FALSE;
	while (!banned && fgets(line, sizeof(line), file))
	{
		char const *ip = strstr(line, "ip=");
		char const *hwid = strstr(line, "hwid=");
		size_t length = csstrlen(text);
		size_t hardware_id_length = csstrlen(kept_hardware_id);

		/* (the whole address: 1.2.3.4 is not 1.2.3.45) */
		if (length && ip && !strncmp(ip + 3, text, length) &&
			(ip[3 + length] == '\t' || ip[3 + length] == '\n' || ip[3 + length] == '\r' ||
				ip[3 + length] == ' ' || ip[3 + length] == 0))
		{
			banned = TRUE;
		}
		/* (or the whole hardware id) */
		if (hardware_id_length && hwid && !strncmp(hwid + 5, kept_hardware_id, hardware_id_length) &&
			(hwid[5 + hardware_id_length] == '\t' || hwid[5 + hardware_id_length] == '\n' ||
				hwid[5 + hardware_id_length] == '\r' || hwid[5 + hardware_id_length] == ' ' ||
				hwid[5 + hardware_id_length] == 0))
		{
			banned = TRUE;
		}
	}
	fclose(file);
	return banned;
}

/* (the host: network_server_manager.c, its ban command) a machine banned
by the host: its line in BANS_FILE, and every machine told (the Discord
user of the machine at the index, if it is one in the game; its address,
host byte order) */
void network_distributed_ban(
	long machine_index,
	unsigned long address,
	char const *names)
{
	char text[32];
	char kept_names[64];
	char discord_id[DISCORD_ID_SIZE] = "";
	char discord_name[DISCORD_NAME_SIZE] = "";
	char discord[DISCORD_ID_SIZE + DISCORD_NAME_SIZE + 16] = "";
	char notice[MAXIMUM_NOTICE_LENGTH];

	distributed_address_text(address, text, sizeof(text));
	distributed_write_player_record(BANS_FILE, text, machine_index, names, "banned by the host");
	distributed_printable(kept_names, sizeof(kept_names), names);
	if (game_in_progress() && machine_index >= 0 && machine_index < HALO_PORT_MAXIMUM_NETWORK_MACHINES)
	{
		p2p_discord_sanitize(discord_id, sizeof(discord_id), distributed_client_identities[machine_index].discord_id, 0);
		p2p_discord_sanitize(discord_name, sizeof(discord_name),
			distributed_client_identities[machine_index].discord_name, 1);
	}
	if (discord_id[0] || discord_name[0])
		snprintf(discord, sizeof(discord), " (Discord: %s, %s)", discord_name, discord_id);
	snprintf(notice, sizeof(notice), "%s%s banned by the host", kept_names, discord);
	/* (to every client in the game: in the lobby, the host's own) */
	if (game_in_progress())
		distributed_send_notice(notice);
	else
	{
		console_warning("%s", notice);
		error(_error_log, "%s", notice);
	}
}

/* (the host) a client machine's tick, which one of its messages is
stamped with: its clock measured, each window, against the host's; one
whose game runs fast (distributed_client_clock) has its players'
predictions refused, and if it goes on, is dropped */
static void distributed_note_client_clock(
	long machine_index,
	long tick)
{
	struct distributed_client_clock *clock;
	unsigned long now = system_milliseconds();
	unsigned long elapsed;

	if (machine_index < 0 || machine_index >= HALO_PORT_MAXIMUM_NETWORK_MACHINES)
		return;
	clock = &distributed_client_clocks[machine_index];
	if (clock->window_tick == NONE)
	{
		clock->latest_tick = tick;
		clock->window_tick = tick;
		clock->window_time = now;
		return;
	}
	if (tick > clock->latest_tick)
		clock->latest_tick = tick;
	elapsed = now - clock->window_time;
	if (elapsed < CLIENT_CLOCK_WINDOW_MILLISECONDS)
		return;
	{
		real rate = (real)(clock->latest_tick - clock->window_tick) /
			((real)elapsed * (real)TICKS_PER_SECOND / 1000.0f);
		long ahead = clock->latest_tick - game_time_get();

		clock->fast = rate > CLIENT_CLOCK_FAST_RATE && ahead > CLIENT_CLOCK_AHEAD_TICKS;
		if (!clock->fast)
		{
			clock->fast_windows = 0;
		}
		else if (++clock->fast_windows == 1)
		{
			error(_error_log, "machine #%ld's game runs %.2f times as fast as this host's (%ld ticks ahead): "
				"its players' predictions refused", machine_index, rate, ahead);
		}
		else if (clock->fast_windows >= CLIENT_CLOCK_FAST_WINDOWS)
		{
			error(_error_log, "machine #%ld's game ran %.2f times as fast as this host's for %d seconds "
				"(%ld ticks ahead): dropped", machine_index, rate,
				CLIENT_CLOCK_FAST_WINDOWS * CLIENT_CLOCK_WINDOW_MILLISECONDS / 1000, ahead);
			{
				char names[64];
				char discord_id[DISCORD_ID_SIZE];
				char discord_name[DISCORD_NAME_SIZE];
				char discord[DISCORD_ID_SIZE + DISCORD_NAME_SIZE + 16] = "";
				char reason[64];
				char text[MAXIMUM_NOTICE_LENGTH];

				distributed_machine_player_names(machine_index, names, sizeof(names));
				p2p_discord_sanitize(discord_id, sizeof(discord_id),
					distributed_client_identities[machine_index].discord_id, 0);
				p2p_discord_sanitize(discord_name, sizeof(discord_name),
					distributed_client_identities[machine_index].discord_name, 1);
				if (discord_id[0] || discord_name[0])
					snprintf(discord, sizeof(discord), " (Discord: %s, %s)", discord_name, discord_id);
				snprintf(reason, sizeof(reason), "speed hack (game ran %.2f times as fast)", rate);
				snprintf(text, sizeof(text), "%s%s kicked by the host: their game ran %.2f times as fast (a speed hack)",
					names, discord, rate);
				distributed_send_notice(text);
				distributed_log_cheater(machine_index, names, reason);
			}
			network_game_server_kick_machine(machine_index);
			clock->fast_windows = 0;
		}
	}
	clock->window_tick = clock->latest_tick;
	clock->window_time = now;
}

/* whether a client machine's game runs fast (distributed_note_client_clock):
its players' predictions not taken */
boolean distributed_machine_clock_fast(
	long machine_index)
{
	return machine_index >= 0 && machine_index < HALO_PORT_MAXIMUM_NETWORK_MACHINES &&
		distributed_client_clocks[machine_index].fast;
}

/* a message of the distributed kind; machine_index is the sender's on the
host, NONE on a client */
void network_distributed_handle_message(
	long machine_index,
	word const *message,
	word size)
{
	struct distributed_message_header header;
	void const *entries = (byte const *)message + sizeof(header);
	short index;
	word entry_size;

	/* (none between games: loading, or in the menus) */
	if (size < sizeof(header) || !game_in_progress())
		return;
	csmemcpy(&header, message, sizeof(header));
	/* a tick's messages in one: each as if it came alone */
	if (header.type == _distributed_message_batch)
	{
		word offset = sizeof(header);

		while (offset + sizeof(word) <= size)
		{
			word buffer[(sizeof(message_header) + DATAGRAM_MAXIMUM_SIZE + 1) / sizeof(word)];
			word length;

			csmemcpy(&length, (byte const *)message + offset, sizeof(word));
			offset += sizeof(word);
			if (length > size - offset || length < sizeof(header) - sizeof(message_header) ||
				sizeof(message_header) + length > sizeof(buffer))
			{
				break;
			}
			csmemcpy(buffer, message, sizeof(message_header));
			csmemcpy((byte *)buffer + sizeof(message_header), (byte const *)message + offset, length);
			offset += length;
			/* (no batch in a batch) */
			if (((struct distributed_message_header const *)buffer)->type != _distributed_message_batch)
				network_distributed_handle_message(machine_index, buffer, (word)(sizeof(message_header) + length));
		}
		return;
	}
	/* (entries of a size of their own: their least here, and each read no
	further than the message's end) */
	switch (header.type)
	{
	case _distributed_message_player_prediction:
	case _distributed_message_unit_states: entry_size = DISTRIBUTED_UNIT_STATE_MINIMUM_SIZE; break;
	case _distributed_message_player_statistics: entry_size = sizeof(struct distributed_player_statistics); break;
	case _distributed_message_pings: entry_size = sizeof(struct distributed_player_ping); break;
	case _distributed_message_pickups: entry_size = sizeof(struct distributed_pickup); break;
	case _distributed_message_player_inputs: entry_size = sizeof(struct distributed_player_input); break;
	case _distributed_message_relayed_actions: entry_size = DISTRIBUTED_RELAYED_ACTION_MINIMUM_SIZE; break;
	case _distributed_message_game_state:
	case _distributed_message_objects_synchronized:
	case _distributed_message_notice:
	case _distributed_message_client_ready: entry_size = 0; break;
	case _distributed_message_client_identity: entry_size = sizeof(struct distributed_client_identity); break;
	case _distributed_message_damage_events:
	case _distributed_message_hit_reports: entry_size = network_damage_entry_size(header.type); break;
	default: entry_size = network_objects_entry_size(header.type); break;
	}
	if (header.type == 0 || header.type >= NUMBER_OF_DISTRIBUTED_MESSAGES ||
		size < sizeof(header) + header.count * entry_size)
	{
		return;
	}
	distributed_statistics.received++;

	/* (each kind from the host, or from a client) */
	switch (header.type)
	{
	case _distributed_message_player_prediction:
	case _distributed_message_client_ready:
	case _distributed_message_client_identity:
	case _distributed_message_hit_reports:
	case _distributed_message_vehicle_prediction:
	case _distributed_message_player_inputs:
		if (machine_index == NONE || game_connection() != _game_connection_network_server)
			return;
		break;
	default:
		if (game_connection() != _game_connection_network_client)
			return;
		/* (the host's latest tick, which this client's input messages tell
		it back) */
		if (distributed_host_time == NONE || header.game_time > distributed_host_time)
			distributed_host_time = header.game_time;
		break;
	}
	if (distributed_message_stale(machine_index, &header))
		return;
	/* (the host: a client's clock, by its messages' ticks; and its players'
	predictions not taken while its game runs fast) */
	if (machine_index != NONE)
	{
		distributed_note_client_clock(machine_index, header.game_time);
		if ((header.type == _distributed_message_player_prediction ||
				header.type == _distributed_message_vehicle_prediction) &&
			distributed_machine_clock_fast(machine_index))
		{
			return;
		}
	}

	switch (header.type)
	{
	case _distributed_message_player_prediction:
		distributed_handle_predictions(machine_index, header.game_time, (byte const *)entries,
			(byte const *)message + size, header.count);
		break;
	case _distributed_message_unit_states:
		distributed_handle_unit_states((byte const *)entries, (byte const *)message + size, header.count);
		break;
	case _distributed_message_player_statistics:
	{
		/* the host's count of kills, deaths, ... */
		struct distributed_player_statistics const *players = (struct distributed_player_statistics const *)entries;

		for (index = 0; index < header.count; index++)
		{
			struct player_datum *player = distributed_player(players[index].player_index);

			if (player)
				player->statistics = players[index].statistics;
		}
		break;
	}
	case _distributed_message_pings:
	{
		/* the host's measure of each player's ping */
		struct distributed_player_ping ping;

		for (index = 0; index < header.count; index++)
		{
			csmemcpy(&ping, (byte const *)entries + index * sizeof(ping), sizeof(ping));
			if (ping.player_index < MAXIMUM_TRACKED_PLAYERS)
				distributed_player_pings[ping.player_index] = ping.milliseconds;
		}
		break;
	}
	case _distributed_message_inventories:
		network_objects_handle_inventories(entries, header.count);
		break;
	case _distributed_message_object_changes:
		network_objects_handle_changes(entries, header.count);
		break;
	case _distributed_message_object_states:
		network_objects_handle_states(entries, header.count);
		break;
	case _distributed_message_game_state:
		game_engine_read_network_state((byte const *)entries, size - sizeof(header));
		break;
	case _distributed_message_objects_synchronized:
		network_objects_handle_synchronized();
		break;
	case _distributed_message_client_identity:
		/* (a client's Discord user, as it tells it: kept only of what is
		allowed, whatever it sent) */
		if (header.count >= 1 && machine_index >= 0 && machine_index < HALO_PORT_MAXIMUM_NETWORK_MACHINES)
		{
			struct distributed_client_identity identity;

			csmemcpy(&identity, entries, sizeof(identity));
			identity.discord_id[sizeof(identity.discord_id) - 1] = 0;
			identity.discord_name[sizeof(identity.discord_name) - 1] = 0;
			p2p_discord_sanitize(distributed_client_identities[machine_index].discord_id,
				sizeof(distributed_client_identities[machine_index].discord_id), identity.discord_id, 0);
			p2p_discord_sanitize(distributed_client_identities[machine_index].discord_name,
				sizeof(distributed_client_identities[machine_index].discord_name), identity.discord_name, 1);
		}
		break;
	case _distributed_message_notice:
	{
		/* the host's text, in red on the console: printable, and ended */
		char text[MAXIMUM_NOTICE_LENGTH];
		long length = size - sizeof(header);
		long index;

		if (length > MAXIMUM_NOTICE_LENGTH - 1)
			length = MAXIMUM_NOTICE_LENGTH - 1;
		csmemcpy(text, entries, length);
		text[length] = 0;
		for (index = 0; index < length && text[index]; index++)
		{
			if (text[index] < 32 || text[index] > 126)
				text[index] = '?';
		}
		console_warning("%s", text);
		error(_error_log, "the host: %s", text);
		break;
	}
	case _distributed_message_client_ready:
	{
		boolean loaded = distributed_machine_loaded(machine_index);

		/* (its count 1: the client asks again, having failed to make one of
		the host's objects) */
		network_objects_client_asked(machine_index, header.count != 0);
		/* (a machine new at its index, which may have joined the game in
		progress: every player's statistics and the game type's state, after
		the objects it names) */
		if (loaded)
		{
			distributed_send_all_statistics(machine_index);
			distributed_send_game_state(machine_index);
		}
		break;
	}
	case _distributed_message_damage_events:
		network_damage_handle_events(entries, header.count);
		break;
	case _distributed_message_hit_reports:
		network_damage_handle_reports(machine_index, entries, header.count);
		break;
	case _distributed_message_vehicle_prediction:
		network_objects_handle_vehicle_prediction(machine_index, entries, header.count);
		break;
	case _distributed_message_player_inputs:
		distributed_handle_inputs(machine_index, (struct distributed_player_input const *)entries, header.count);
		break;
	case _distributed_message_relayed_actions:
		distributed_handle_actions((byte const *)entries, (byte const *)message + size, header.count);
		break;
	case _distributed_message_pickups:
	{
		/* what the host says this machine's players picked up */
		struct distributed_pickup const *pickups = (struct distributed_pickup const *)entries;

		for (index = 0; index < header.count; index++)
		{
			long player_index = distributed_player_from_byte(pickups[index].player_index);

			if (distributed_player_is_local(player_index))
			{
				network_player_show_pickup(player_index, pickups[index].kind, pickups[index].definition_index,
					pickups[index].count);
			}
		}
		break;
	}
	}
}
