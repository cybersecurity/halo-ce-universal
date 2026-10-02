/*
NETWORK_OBJECTS.C

The distributed netcode's objects (port/linux/NETCODE.md, stages 3 and 4).

The game's objects that matter to more than the eye (units, vehicles,
weapons and equipment) are the host's, and every machine has them at the
same datum index (identifier and all), so that any message can name one:

- The host tells its clients (reliably) of each object it makes, with what
  it is, where, and how it looks, and of each it deletes; a client makes
  and deletes its copies to match, at the host's index. A client that has
  loaded asks for the host's objects all over and is told when it has them
  all; from then on it deletes any such object it has that the host has not
  told it of (what its own simulation made: a dying unit's grenades, a
  vehicle's weapons), and nothing but the host's word deletes the host's.
- A client places the game's objects when its map loads as the host did,
  at the same indices: the host's word on those finds them already there.
  Past loading, its own objects (projectiles, effects' objects: what only
  it sees) take indices from the upper half of the array, which the host's
  do not reach in practice; a host's object that does comes first.
- Every tick the host sends where its moving objects are (vehicles, items,
  bodies), each client those near its players every tick and those further
  less often, with each as it comes to rest, and to every client a few of
  those at rest, round the lot; a client moves its copies there, a little
  off half of the way each tick, further (or come to rest) drawn gliding from
  where they were rather than jumping (render_interpolation.c).
  A client's own player's vehicle is its own, as its own player's unit is:
  it sends the host where it drives it at which of its ticks, which the host
  takes within what the vehicle can have moved at its next tick, and tells
  the client which of its ticks it has it at; the client moves it by how far
  that is from where it had it at that tick, only when far off.
- Ten times a second, the host sends what a unit carries when that has
  changed (and once a second whatever it is): which weapons, slot for slot,
  their ammunition, the weapon in hand and the grenades; a client moves its
  copies of those weapons in and out of its copies of the units to match (it
  decides no pickups, swaps or drops itself). A change of weapons or
  grenades goes to every client at once, one of ammunition alone to the
  unit's own client at once and to the others as often as the unit's place
  is sent them. A client's own player spends its own ammunition and grenades:
  it takes the host's count only once the host has had a round trip to see
  what it did.
*/

#include "cseries.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "networking/network_game_globals.h"
#include "objects/objects.h"
#include "objects/damage.h"
#include "objects/object_definitions.h"
#include "models/model_definitions.h"
#include "units/units.h"
#include "units/biped_definitions.h"
#include "units/vehicle_definitions.h"
#include "units/vehicle_datum.h"
#include "items/items.h"
#include "items/weapons.h"
#include "items/projectiles.h"
#include "items/weapon_definitions.h"
#include "items/equipment_definitions.h"
#include "network_distributed.h"

#include <math.h>

/* physics.c's */
extern real global_gravity;

/* units.c's */
void unit_network_add_weapon(long unit_index, long weapon_index, short slot);
void unit_network_drop_weapon(long unit_index, short slot);
void unit_network_forget_weapon(long unit_index, short slot);
/* players.c's */
void network_player_detach_unit(long player_index);
/* render_interpolation.c's */
void render_interpolation_correct_object(long object_index, real_vector3d const *offset);
/* cache_files.c's */
boolean tag_index_is_group(long tag_index, long group_tag);

/* the vehicle tag's speeds (world units a tick), as actor_moving.c reads
them: its layout is not the public header's */
struct vehicle_definition
{
	byte __unknown0[0x2F4];
	short vehicle_type;
	short __unknown2F6;
	real maximum_forward_speed;
	real maximum_reverse_speed;
};

typedef char network_objects_vehicle_type_offset_assert[
	offsetof(struct vehicle_definition, vehicle_type) == 0x2F4 ? 1 : -1];
/* (vehicles.c's vehicle types: those that float, fly or stay) */
enum
{
	_vehicle_type_human_boat = 2,
	_vehicle_type_human_plane = 3,
	_vehicle_type_alien_fighter = 5,
	_vehicle_type_turret = 6,
};

typedef char network_objects_vehicle_maximum_forward_speed_offset_assert[
	offsetof(struct vehicle_definition, maximum_forward_speed) == 0x2F8 ? 1 : -1];

enum
{
	INVENTORY_INTERVAL_TICKS = 3,
	/* a unit's inventory is sent this often even unchanged (one lost is
	made good) */
	INVENTORY_REFRESH_TICKS = TICKS_PER_SECOND,
	/* ... and a unit's that has come to carry nothing, for this long after */
	EMPTY_INVENTORY_TICKS = 3 * INVENTORY_REFRESH_TICKS,
	/* the host's objects at rest sent each tick, round them all */
	RESTING_STATES_PER_TICK = 4,
	/* a client asks for the host's objects again until it has them (a host
	still loading misses the asking), after this, twice as long each time
	up to the most */
	CLIENT_READY_INTERVAL_TICKS = TICKS_PER_SECOND,
	CLIENT_READY_MAXIMUM_INTERVAL_TICKS = 4 * TICKS_PER_SECOND,
	/* the host tells a machine all its objects again at most once in this
	long, asked again (a client that failed to make one: asked again sooner,
	they are on their way; a flood of asking is not answered in full) */
	HOST_OBJECTS_RESEND_TICKS = 10 * TICKS_PER_SECOND,
	/* a client that failed to make one of the host's objects asks for them
	all again this long after, twice as long each time it fails again up to
	the most */
	CLIENT_RETRY_TICKS = HOST_OBJECTS_RESEND_TICKS + TICKS_PER_SECOND,
	CLIENT_RETRY_MAXIMUM_TICKS = 60 * TICKS_PER_SECOND,
	/* a client's own objects, from here up */
	LOCAL_OBJECTS_FIRST_INDEX = MAXIMUM_OBJECTS_PER_MAP / 2,
	/* ... those of the kinds the host has, made since the last tick, checked
	(more, and all of them are) */
	MAXIMUM_CLIENT_NEW_OBJECTS = 1024,
	MAXIMUM_ENTRIES_PER_MESSAGE = 64,
	/* the objects the host has at the same indices everywhere */
	NETWORKED_OBJECT_TYPES =
		_object_mask_biped | _object_mask_vehicle | _object_mask_weapon | _object_mask_equipment,
	/* how often the host sends a client a moving object further from its
	players than each distance below (every tick nearer), and the vehicle the
	client drives itself */
	MAXIMUM_OBJECT_PERIOD_TICKS = 4,
	OWN_VEHICLE_PERIOD_TICKS = 3,
	/* a client: where the vehicles its own players drive were, the last
	ticks (a power of two, more than the longest round trip) */
	OWN_VEHICLE_POSITION_TICKS = 64,
	/* the host: the client's ticks it takes as run since a client's vehicle's
	anchor beyond its own (its messages delayed, then bunched) */
	VEHICLE_PREDICTION_JITTER_TICKS = 6,
	/* ... how much higher than a throw takes it a vehicle in the air may be
	(its bounces, the ground's bumps), world units */
	VEHICLE_PREDICTION_RISE_TOLERANCE = 2,
	/* ... and the most of a client's round trip it takes a client's vehicle
	in the air to be ahead of its copy by */
	VEHICLE_PREDICTION_CEILING_LEAD_TICKS = 15,
	/* ... how long it measures predictions from one it took, the anchor,
	before it takes a newer as the anchor (the jitter and the blend granted
	once a second, not once a tick) */
	VEHICLE_PREDICTION_ANCHOR_TICKS = TICKS_PER_SECOND,
	/* ... and how long, with none taken, it keeps the anchor, and tells the
	client which of its ticks it has the vehicle at (longer: from where it
	has the vehicle only, a ride begun again) */
	VEHICLE_PREDICTION_REFERENCE_TICKS = 2 * TICKS_PER_SECOND,
	/* ... and how long it remembers how fast its own ticks sent a client's
	vehicle (an anchor's second, and the longest round trip: a client learns
	of an explosion that throws its vehicle a round trip late) */
	VEHICLE_PREDICTION_SPEED_TICKS = VEHICLE_PREDICTION_ANCHOR_TICKS + 2 * TICKS_PER_SECOND,
	/* a client: how long a biped the host says is dead, no player's, may
	live on here before it is killed with nothing to show (the host's
	killing blow, unreliable, shows it; lost, nothing else would) */
	DEAD_BIPED_FALLBACK_TICKS = TICKS_PER_SECOND / 2,
	/* a client: its own player's ammunition and grenades are the host's once
	the host has had this long past a round trip to see what it did with
	them (the host's inventories go every INVENTORY_INTERVAL_TICKS) */
	OWN_INVENTORY_MARGIN_TICKS = 2,
	/* ... the round trip without one measured, and the most */
	DEFAULT_OWN_ROUND_TRIP_TICKS = 6,
	MAXIMUM_OWN_ROUND_TRIP_TICKS = 60,
	/* the most grenades of a kind a unit carries, as the host says (a dead
	unit drops each) */
	MAXIMUM_INVENTORY_GRENADES = 16,
};

/* the distances from a client's nearest player (world units) past which
the host sends it a moving object every second tick, every third and every
fourth (as it sends players, network_distributed.c) */
#define NEAR_OBJECT_DISTANCE 25.0f
#define MIDDLE_OBJECT_DISTANCE 60.0f
#define FAR_OBJECT_DISTANCE 120.0f

/* world units */
#define REMOTE_OBJECT_TOLERANCE 0.05f
/* ... and the cosine of the angle (an object at rest turned) */
#define REMOTE_OBJECT_ANGLE_TOLERANCE 0.98f
/* how far a client's vehicle prediction moves the host's copy for the copy
at rest to wake, world units */
#define VEHICLE_PREDICTION_STILL_DISTANCE 0.01f
#define LOCAL_VEHICLE_TOLERANCE 4.0f
/* (the host takes further than a client puts right: between the two they
would disagree for good) */
#define HOST_VEHICLE_ACCEPT_TOLERANCE 5.0f
/* how far from the origin an object is (world units) */
#define OBJECT_WORLD_BOUND 32768.0f
/* how fast a client's own player's vehicle moves (world units, radians a
tick) */
#define MAXIMUM_PREDICTED_VEHICLE_SPEED 3.0f
#define MAXIMUM_PREDICTED_VEHICLE_ANGULAR_SPEED 1.0f
/* ... how much further a tick than it goes (twice its tag's top speed, or
as fast as the host's ticks sent its copy lately) it may be (world units a
tick) */
#define PREDICTED_VEHICLE_SPEED_MARGIN 0.1f
/* the teams a flag's or ball's owner team names (game_engine_ctf.c's
NUMBER_OF_CTF_TEAMS, game_engine_oddball.c's MAXIMUM_ODDBALLS) */
#define CTF_FLAG_TEAMS 2
#define ODDBALL_TEAMS 16

enum
{
	_object_change_create,
	_object_change_delete,
};

/* struct distributed_object_change and struct distributed_object_state
flags */
enum
{
	/* an item in a unit's inventory, not in the world */
	_distributed_object_carried_bit = 0,
	_distributed_object_at_rest_bit,
	/* a unit dead */
	_distributed_object_dead_bit,
	/* (struct distributed_object_state, the host's) the host has the vehicle
	where the client it goes to had it at the tick in time */
	_distributed_object_predicted_bit,
};

struct distributed_object_change
{
	byte change;
	byte flags;
	byte owner_player_index;
	byte pad;
	long object_index;
	long definition_index;
	short owner_team_index;
	short variant_number;
	real_point3d position;
	real_vector3d forward;
	real_vector3d up;
	real_vector3d translational_velocity;
	real_vector3d angular_velocity;
	real_rgb_color change_colors[NUMBER_OF_OBJECT_CHANGE_COLORS];
	byte region_permutations[MAXIMUM_REGIONS_PER_OBJECT];
};

struct distributed_object_state
{
	long object_index;
	byte flags;
	byte pad;
	/* the client's tick, its low 16 bits: a vehicle prediction's, and
	(_distributed_object_predicted_bit) the one the host has it at */
	short time;
	real_point3d position;
	struct distributed_vector forward;
	struct distributed_vector up;
	struct distributed_vector translational_velocity;
	struct distributed_vector angular_velocity;
};

struct distributed_inventory
{
	long unit_index;
	char grenade_counts[NUMBER_OF_UNIT_GRENADE_TYPES];
	char current_weapon_index;
	byte pad;
	long weapon_indices[MAXIMUM_WEAPONS_PER_UNIT];
	/* each weapon's magazines */
	short rounds_total[MAXIMUM_WEAPONS_PER_UNIT][2];
	short rounds_loaded[MAXIMUM_WEAPONS_PER_UNIT][2];
	/* (of 0 to 1, AGE_SCALE) */
	word age[MAXIMUM_WEAPONS_PER_UNIT];
};

#define AGE_SCALE 65535.0f

struct distributed_object_change_message
{
	struct distributed_message_header header;
	struct distributed_object_change changes[MAXIMUM_ENTRIES_PER_MESSAGE];
};

struct distributed_object_state_message
{
	struct distributed_message_header header;
	struct distributed_object_state states[MAXIMUM_ENTRIES_PER_MESSAGE];
};

struct distributed_inventory_message
{
	struct distributed_message_header header;
	struct distributed_inventory inventories[MAXIMUM_ENTRIES_PER_MESSAGE];
};

/* ---------- globals */

/* the host: the objects it has told its clients of, by absolute index
(the object's datum index), NONE for none */
static long objects_host_told[MAXIMUM_TRACKED_OBJECTS];
/* ... the next object at rest to send, round them all */
static long objects_host_resting_cursor;
/* ... past the highest of them */
static long objects_host_told_count;
/* ... whether each was moving at the last tick (one come to rest is sent
once more, to every client) */
static boolean objects_host_state_moving[MAXIMUM_TRACKED_OBJECTS];
/* ... what each unit's inventory was last sent as (in all, and its weapons
and grenades), when to every client, and when its ammunition alone last
changed (NONE: not since); when it last carried anything (NONE: never) */
static struct
{
	unsigned long checksum;
	unsigned long weapons_checksum;
	long time;
	long ammunition_time;
	long carried_time;
} objects_host_inventories[MAXIMUM_TRACKED_OBJECTS];
/* ... each client machine, found once a tick: where its players' living
units are, those units, and the vehicles they drive (with the player's
absolute index) */
static struct
{
	short count;
	long indices[HALO_PORT_MAXIMUM_NETWORK_MACHINES];
	struct
	{
		short unit_count;
		long unit_indices[MAXIMUM_LOCAL_PLAYERS];
		real_point3d origins[MAXIMUM_LOCAL_PLAYERS];
		short vehicle_count;
		long vehicle_indices[MAXIMUM_LOCAL_PLAYERS];
		short vehicle_player_indices[MAXIMUM_LOCAL_PLAYERS];
	} machines[HALO_PORT_MAXIMUM_NETWORK_MACHINES];
} objects_host_viewers;
/* ... each machine it has told all its objects: when, and which players it
had then (another machine in its place has others) */
static struct
{
	long time;
	long player_indices[MAXIMUM_LOCAL_PLAYERS];
} objects_host_machines[HALO_PORT_MAXIMUM_NETWORK_MACHINES];
/* ... each client's player's latest vehicle prediction, taken at the next
tick; the vehicle it last took (NONE: none), at which of the client's ticks
and its own, and where that left the host's copy; and the anchor
predictions are measured from: one it took, at which of the client's ticks
and its own, and where the client had it; the velocity it took (what its
next tick started from); and how fast its own ticks sent the vehicle beyond
the client's word, the most of this span of VEHICLE_PREDICTION_SPEED_TICKS
and of the one before */
static struct
{
	boolean valid;
	long machine_index;
	struct distributed_object_state state;
	long accepted_vehicle_index;
	word accepted_time;
	long accepted_host_time;
	real_point3d accepted_host_position;
	word anchor_time;
	long anchor_host_time;
	real_point3d anchor_position;
	real_vector3d accepted_velocity;
	long speed_time;
	real speeds[2];
	/* the vehicle it last had on the ground (NONE: none), how high, and how
	fast up it went as it left it, and when (network_objects_note_vehicle_ground) */
	long ground_vehicle_index;
	long ground_time;
	/* the vehicle whose copy was at rest when it last took its word (NONE:
	none), and where it came to rest */
	long rest_vehicle_index;
	real_point3d rest_position;
	real ground_height;
	real ground_rise_speed;
} objects_host_vehicle_predictions[MAXIMUM_TRACKED_PLAYERS];
/* a client: the host's objects it has, by absolute index */
static long objects_client_has[MAXIMUM_TRACKED_OBJECTS];
/* ... the bipeds, no player's, the host said were dead while they lived
here, and when it first did (DEAD_BIPED_FALLBACK_TICKS) */
static struct
{
	long object_index;
	long time;
} objects_client_dead[MAXIMUM_TRACKED_OBJECTS];
/* ... its own objects made since the last tick (of any kind: those of the
kinds the host has go), and whether to look through all the objects
instead (the host's all told, too many made, or made where it cannot say) */
static long objects_client_new_objects[MAXIMUM_CLIENT_NEW_OBJECTS];
static short objects_client_new_object_count;
static boolean objects_client_check_all;
/* ... the lowest of its own indices that may be free (those below it
taken), and the object array's count when it last made an object (NONE:
not known; another count, an object deleted since: from the first) */
static long objects_client_local_free_index;
static long objects_client_object_count;
/* ... where the vehicles its own players drive were at its last ticks */
static struct distributed_own_vehicle
{
	long time;
	long vehicle_index;
	real_point3d position;
} objects_client_own_vehicles[MAXIMUM_LOCAL_PLAYERS][OWN_VEHICLE_POSITION_TICKS];
/* ... what its own players' units carried at the last tick, and when they
last threw a grenade of each kind, and fired or reloaded each weapon
(NONE: not while it has had it) */
static struct distributed_own_inventory
{
	long unit_index;
	char grenade_counts[NUMBER_OF_UNIT_GRENADE_TYPES];
	long grenade_times[NUMBER_OF_UNIT_GRENADE_TYPES];
	long weapon_indices[MAXIMUM_WEAPONS_PER_UNIT];
	short rounds[MAXIMUM_WEAPONS_PER_UNIT][2][2];
	long fired_times[MAXIMUM_WEAPONS_PER_UNIT];
	long weapon_times[MAXIMUM_WEAPONS_PER_UNIT];
} objects_client_own_inventories[MAXIMUM_LOCAL_PLAYERS];
/* ... whether it failed to make one of the host's objects since it last
asked for them (it says so when it asks again) */
static boolean objects_client_ask_again;
/* ... all of them (the host said so), when it last asked for them and how
long until it asks again */
static boolean objects_client_synchronized;
static long objects_client_ready_time;
/* During reattachment keep the surviving world until the host has replayed
its current object identities. Then retire only generations absent from it. */
static boolean objects_client_resynchronizing;
static boolean objects_client_resync_seen[MAXIMUM_TRACKED_OBJECTS];
static long objects_client_ready_interval;
/* ... when it last failed to make one (NONE: not since the host last told
it all), and how long it waits after the next failure */
static long objects_client_failed_time;
static long objects_client_retry_ticks;
/* ... past loading: its own objects from the upper half */
static boolean objects_client_local_allocation;
static short objects_client_local_identifier;
/* ... making the host's object at the host's index, NONE for none */
static long objects_client_creating_index = NONE;
static boolean objects_client_creating;
/* ... deleting one on the host's word */
static boolean objects_client_deleting;

/* for the automated tests' reports (network_test.c) */
static struct
{
	long creates;
	long deletes;
	long create_failures;
	long own_objects_removed;
} objects_statistics;

void network_distributed_item_statistics(
	long *creates,
	long *deletes,
	long *failures,
	long *removed)
{
	*creates = objects_statistics.creates;
	*deletes = objects_statistics.deletes;
	*failures = objects_statistics.create_failures;
	*removed = objects_statistics.own_objects_removed;
}

/* ---------- objects (objects.c) */

/* the datum index a new object takes: the host's, for its object a client
makes, or a client's own from the upper half; NONE for the first free */
long network_objects_new_object_index(
	void)
{
	long object_index = objects_client_creating_index;
	long object_count = object_header_data->actual_count;
	long absolute_index;

	/* (every object is made just after this: one more since the last, and
	none has been deleted, the indices below the lowest free one are still
	taken; an object made or deleted otherwise, and they are looked through
	from the first again) */
	if (objects_client_object_count == NONE || object_count != objects_client_object_count + 1)
		objects_client_local_free_index = LOCAL_OBJECTS_FIRST_INDEX;
	objects_client_object_count = object_count;
	if (object_index != NONE)
	{
		objects_client_creating_index = NONE;
		return object_index;
	}
	if (!objects_client_local_allocation || !network_game_distributed_client())
	{
		objects_client_object_count = NONE;
		return NONE;
	}
	for (absolute_index = MAX(objects_client_local_free_index, LOCAL_OBJECTS_FIRST_INDEX);
		absolute_index < object_header_data->maximum_count; absolute_index++)
	{
		struct datum_header const *header = (struct datum_header const *)
			((byte const *)object_header_data->data + absolute_index * object_header_data->size);

		if (!header->identifier)
		{
			objects_client_local_free_index = absolute_index;
			/* (any identifier but 0, which marks a free datum) */
			if (++objects_client_local_identifier == 0)
				objects_client_local_identifier = 1;
			object_index = ((long)objects_client_local_identifier << 16) | absolute_index;
			/* (of the kinds the host has, it goes: looked at next tick) */
			if (objects_client_new_object_count < MAXIMUM_CLIENT_NEW_OBJECTS)
				objects_client_new_objects[objects_client_new_object_count++] = object_index;
			else
				objects_client_check_all = TRUE;
			return object_index;
		}
	}
	/* (the upper half full: the first free, anywhere) */
	objects_client_local_free_index = object_header_data->maximum_count;
	objects_client_check_all = TRUE;
	return NONE;
}

/* whether a client is making the host's object: which the host has made as
the game type has it (object_new does not remap it again), with its own
parts (a unit's initial weapons are the host's objects too) */
boolean network_objects_creating_host_object(
	void)
{
	return objects_client_creating;
}

/* whether the object may be deleted: not the host's on a client, but on
the host's word */
boolean network_objects_may_delete(
	long object_index)
{
	long absolute_index = DATUM_INDEX_TO_ABSOLUTE_INDEX(object_index);

	return objects_client_deleting || !network_game_distributed_client() ||
		absolute_index < 0 || absolute_index >= MAXIMUM_TRACKED_OBJECTS ||
		objects_client_has[absolute_index] != object_index;
}

/* the map's objects placed (game.c): a client's own come from the upper
half from now on */
void network_objects_placed(
	void)
{
	objects_client_local_allocation = network_game_distributed_client();
}

/* ---------- common */

static boolean distributed_object_networked(
	long object_index)
{
	struct object_header_datum *header = object_header_try_and_get(object_index);

	return header && header->datum && TEST_FLAG(NETWORKED_OBJECT_TYPES, header->type) &&
		!TEST_FLAG(header->flags, _object_header_being_deleted_bit) &&
		DATUM_INDEX_TO_ABSOLUTE_INDEX(object_index) < MAXIMUM_TRACKED_OBJECTS;
}

boolean network_objects_client_has(
	long object_index)
{
	long absolute_index = DATUM_INDEX_TO_ABSOLUTE_INDEX(object_index);

	return object_index != NONE && absolute_index >= 0 && absolute_index < MAXIMUM_TRACKED_OBJECTS &&
		objects_client_has[absolute_index] == object_index && object_try_and_get(object_index);
}

word network_objects_entry_size(
	byte type)
{
	switch (type)
	{
	case _distributed_message_object_changes: return sizeof(struct distributed_object_change);
	case _distributed_message_object_states:
	case _distributed_message_vehicle_prediction: return sizeof(struct distributed_object_state);
	case _distributed_message_inventories: return sizeof(struct distributed_inventory);
	}
	return 0;
}

static boolean distributed_vector_valid(
	real_vector3d const *vector)
{
	return distributed_real_valid(vector->i) && distributed_real_valid(vector->j) && distributed_real_valid(vector->k);
}

/* whether a message's transform can be: the position finite and within the
world, the axes a rotation (made exactly one in valid_forward, valid_up),
the velocities finite */
static boolean distributed_transform_valid(
	real_point3d const *position,
	real_vector3d const *forward,
	real_vector3d const *up,
	real_vector3d const *velocity,
	real_vector3d const *angular_velocity,
	real_vector3d *valid_forward,
	real_vector3d *valid_up)
{
	if (!position || !forward || !up || !distributed_point_valid(position, OBJECT_WORLD_BOUND) ||
		(velocity && !distributed_vector_valid(velocity)) ||
		(angular_velocity && !distributed_vector_valid(angular_velocity)))
	{
		return FALSE;
	}
	*valid_forward = *forward;
	*valid_up = *up;
	return distributed_axes_make_valid(valid_forward, valid_up);
}

/* the vector no longer than maximum */
static void distributed_vector_clamp(
	real_vector3d *vector,
	real maximum)
{
	real length = (real)sqrt(vector->i * vector->i + vector->j * vector->j + vector->k * vector->k);

	if (length > maximum)
	{
		vector->i *= maximum / length;
		vector->j *= maximum / length;
		vector->k *= maximum / length;
	}
}

/* (the transform checked) */
static void distributed_object_move(
	long object_index,
	real_point3d const *position,
	real_vector3d const *forward,
	real_vector3d const *up,
	real_vector3d const *velocity,
	real_vector3d const *angular_velocity)
{
	struct object_datum *object = object_get(object_index);
	real_vector3d offset;

	/* (drawn from where it was, the difference fading over a few ticks) */
	offset.i = object->object.position.x - position->x;
	offset.j = object->object.position.y - position->y;
	offset.k = object->object.position.z - position->z;
	object_set_position(object_index, position, forward, up);
	if (velocity)
		object->object.translational_velocity = *velocity;
	if (angular_velocity)
		object->object.angular_velocity = *angular_velocity;
	render_interpolation_correct_object(object_index, &offset);
}

void network_objects_correct(
	long object_index,
	real_point3d const *position,
	real_vector3d const *forward,
	real_vector3d const *up,
	real_vector3d const *velocity,
	real_vector3d const *angular_velocity)
{
	real_vector3d valid_forward, valid_up;

	/* (what cannot be, from a message: not taken) */
	if (distributed_transform_valid(position, forward, up, velocity, angular_velocity, &valid_forward, &valid_up))
		distributed_object_move(object_index, position, &valid_forward, &valid_up, velocity, angular_velocity);
}

boolean network_objects_reconcile(
	long object_index,
	real_point3d const *position,
	real_vector3d const *forward,
	real_vector3d const *up,
	real_vector3d const *velocity,
	real_vector3d const *angular_velocity,
	real blend_distance)
{
	struct object_datum *object = object_get(object_index);
	real_vector3d valid_forward, valid_up;
	real_point3d blended;
	real dx, dy, dz;

	/* (what cannot be, from a message: not taken) */
	if (!distributed_transform_valid(position, forward, up, velocity, angular_velocity, &valid_forward, &valid_up))
		return FALSE;
	dx = position->x - object->object.position.x;
	dy = position->y - object->object.position.y;
	dz = position->z - object->object.position.z;
	/* (so written that its own position gone wrong, not a number, is put
	right) */
	if (!(dx * dx + dy * dy + dz * dz <= blend_distance * blend_distance))
	{
		distributed_object_move(object_index, position, &valid_forward, &valid_up, velocity, angular_velocity);
		return TRUE;
	}
	/* (half of the way: the tick's snapshots draw it moving, no jump) */
	blended.x = object->object.position.x + dx * 0.5f;
	blended.y = object->object.position.y + dy * 0.5f;
	blended.z = object->object.position.z + dz * 0.5f;
	object_set_position(object_index, &blended, &valid_forward, &valid_up);
	if (velocity)
		object->object.translational_velocity = *velocity;
	if (angular_velocity)
		object->object.angular_velocity = *angular_velocity;
	return FALSE;
}

/* an object's state as the host sent it, in full */
static void distributed_object_state_unpack(
	struct distributed_object_state const *state,
	real_vector3d *forward,
	real_vector3d *up,
	real_vector3d *velocity,
	real_vector3d *angular_velocity)
{
	/* (as they came: distributed_transform_valid makes them a rotation or
	refuses them) */
	distributed_vector_unpack(&state->forward, DISTRIBUTED_UNIT_SCALE, forward);
	distributed_vector_unpack(&state->up, DISTRIBUTED_UNIT_SCALE, up);
	distributed_vector_unpack(&state->translational_velocity, DISTRIBUTED_VELOCITY_SCALE, velocity);
	distributed_vector_unpack(&state->angular_velocity, DISTRIBUTED_ANGULAR_VELOCITY_SCALE, angular_velocity);
}

static void distributed_state_from_object(
	long object_index,
	struct distributed_object_state *state)
{
	struct object_datum *object = object_get(object_index);

	csmemset(state, 0, sizeof(*state));
	state->object_index = object_index;
	SET_FLAG(state->flags, _distributed_object_at_rest_bit, TEST_FLAG(object->object.flags, _object_at_rest_bit));
	SET_FLAG(state->flags, _distributed_object_dead_bit, TEST_FLAG(_object_mask_unit, object->object.type) &&
		TEST_FLAG(object->object.damage_flags, _object_dead_bit));
	state->position = object->object.position;
	distributed_vector_pack(&object->object.forward, DISTRIBUTED_UNIT_SCALE, &state->forward);
	distributed_vector_pack(&object->object.up, DISTRIBUTED_UNIT_SCALE, &state->up);
	distributed_vector_pack(&object->object.translational_velocity, DISTRIBUTED_VELOCITY_SCALE,
		&state->translational_velocity);
	distributed_vector_pack(&object->object.angular_velocity, DISTRIBUTED_ANGULAR_VELOCITY_SCALE,
		&state->angular_velocity);
}

/* the vehicle the player's unit drives, or NONE */
static long distributed_driven_vehicle(
	struct player_datum const *player)
{
	long unit_index = distributed_living_unit(player);
	struct unit_datum *vehicle;

	if (unit_index == NONE || object_get(unit_index)->object.parent_object_index == NONE)
		return NONE;
	vehicle = (struct unit_datum *)object_try_and_get_and_verify_type(
		object_get(unit_index)->object.parent_object_index, _object_mask_vehicle);
	return vehicle && vehicle->unit.driver_object_index == unit_index ?
		object_get(unit_index)->object.parent_object_index : NONE;
}

/* ---------- the host */

static void distributed_change_from_object(
	long object_index,
	struct distributed_object_change *change)
{
	struct object_datum *object = object_get(object_index);

	csmemset(change, 0, sizeof(*change));
	change->change = _object_change_create;
	change->object_index = object_index;
	change->definition_index = object->definition_index;
	change->owner_player_index = distributed_player_to_byte(object->object.owner_player_index);
	change->owner_team_index = object->object.owner_team_index;
	change->variant_number = object->object.variant_number;
	change->position = object->object.position;
	change->forward = object->object.forward;
	change->up = object->object.up;
	change->translational_velocity = object->object.translational_velocity;
	change->angular_velocity = object->object.angular_velocity;
	csmemcpy(change->change_colors, object->object.base_change_colors, sizeof(change->change_colors));
	csmemcpy(change->region_permutations, object->object.region_permutations, sizeof(change->region_permutations));
	SET_FLAG(change->flags, _distributed_object_at_rest_bit, TEST_FLAG(object->object.flags, _object_at_rest_bit));
	SET_FLAG(change->flags, _distributed_object_dead_bit, TEST_FLAG(_object_mask_unit, object->object.type) &&
		TEST_FLAG(object->object.damage_flags, _object_dead_bit));
	if (TEST_FLAG(_object_mask_item, object->object.type))
	{
		struct item_datum *item = item_get(object_index);

		SET_FLAG(change->flags, _distributed_object_carried_bit,
			TEST_FLAG(item->item.flags, _item_attached_to_unit_bit) ||
			object->object.parent_object_index != NONE ||
			!TEST_FLAG(object->object.flags, _object_connected_to_map_bit));
	}
}

/* the objects made and deleted since the last time, to every client */
static void distributed_host_update_objects(
	void)
{
	static boolean seen[MAXIMUM_TRACKED_OBJECTS];
	struct distributed_object_change_message message;
	short count = 0;
	short limit = MIN(MAXIMUM_ENTRIES_PER_MESSAGE, RELIABLE_ENTRIES(struct distributed_object_change));
	struct object_iterator iterator;
	long absolute_index;

	/* (none set past those told of) */
	csmemset(seen, 0, objects_host_told_count * sizeof(seen[0]));
	object_iterator_new(&iterator, NETWORKED_OBJECT_TYPES, 0);
	while (object_iterator_next(&iterator))
	{
		if (!distributed_object_networked(iterator.index))
			continue;
		absolute_index = DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.index);
		seen[absolute_index] = TRUE;
		if (objects_host_told[absolute_index] == iterator.index)
			continue;
		/* (another object in the same place: that one is gone) */
		if (objects_host_told[absolute_index] != NONE)
		{
			csmemset(&message.changes[count], 0, sizeof(message.changes[count]));
			message.changes[count].change = _object_change_delete;
			message.changes[count].object_index = objects_host_told[absolute_index];
			objects_statistics.deletes++;
			if (++count == limit)
			{
				distributed_send(&message, _distributed_message_object_changes, count,
					(word)(sizeof(message.header) + count * sizeof(struct distributed_object_change)),
					_distributed_to_clients_reliably);
				count = 0;
			}
		}
		objects_host_told[absolute_index] = iterator.index;
		if (absolute_index >= objects_host_told_count)
			objects_host_told_count = absolute_index + 1;
		objects_statistics.creates++;
		distributed_change_from_object(iterator.index, &message.changes[count]);
		if (++count == limit)
		{
			distributed_send(&message, _distributed_message_object_changes, count,
				(word)(sizeof(message.header) + count * sizeof(struct distributed_object_change)),
				_distributed_to_clients_reliably);
			count = 0;
		}
	}
	for (absolute_index = 0; absolute_index < objects_host_told_count; absolute_index++)
	{
		if (objects_host_told[absolute_index] == NONE || seen[absolute_index])
			continue;
		csmemset(&message.changes[count], 0, sizeof(message.changes[count]));
		message.changes[count].change = _object_change_delete;
		message.changes[count].object_index = objects_host_told[absolute_index];
		objects_host_told[absolute_index] = NONE;
		objects_statistics.deletes++;
		if (++count == limit)
		{
			distributed_send(&message, _distributed_message_object_changes, count,
				(word)(sizeof(message.header) + count * sizeof(struct distributed_object_change)),
				_distributed_to_clients_reliably);
			count = 0;
		}
	}
	if (count)
	{
		distributed_send(&message, _distributed_message_object_changes, count,
			(word)(sizeof(message.header) + count * sizeof(struct distributed_object_change)),
			_distributed_to_clients_reliably);
	}
}

/* a client has loaded the game: every object the host has, to it alone,
and word that that is all of them; asked again, only the word (they are on
their way ahead of it), unless it failed to make one since it last asked
(again) */
void network_objects_client_asked(
	long machine_index,
	boolean again)
{
	struct distributed_object_change_message message;
	short count = 0;
	short limit = MIN(MAXIMUM_ENTRIES_PER_MESSAGE, RELIABLE_ENTRIES(struct distributed_object_change));
	long absolute_index;
	long *player_list;

	if (machine_index < 0 || machine_index >= HALO_PORT_MAXIMUM_NETWORK_MACHINES)
		return;
	player_list = machine_get_player_list(machine_index);
	/* (another machine in its place has other players) */
	if ((again && game_time_get() - objects_host_machines[machine_index].time >= HOST_OBJECTS_RESEND_TICKS) ||
		objects_host_machines[machine_index].time == NONE ||
		csmemcmp(objects_host_machines[machine_index].player_indices, player_list,
			sizeof(objects_host_machines[machine_index].player_indices)) != 0)
	{
		objects_host_machines[machine_index].time = game_time_get();
		csmemcpy(objects_host_machines[machine_index].player_indices, player_list,
			sizeof(objects_host_machines[machine_index].player_indices));
	}
	else
	{
		distributed_send_to_machine_reliably(machine_index, &message, _distributed_message_objects_synchronized, 0,
			(word)sizeof(message.header));
		return;
	}
	distributed_host_update_objects();
	for (absolute_index = 0; absolute_index < objects_host_told_count; absolute_index++)
	{
		/* (what every unit carries, with the next of them) */
		objects_host_inventories[absolute_index].checksum = 0;
		objects_host_inventories[absolute_index].weapons_checksum = 0;
		if (objects_host_told[absolute_index] == NONE)
			continue;
		distributed_change_from_object(objects_host_told[absolute_index], &message.changes[count]);
		if (++count == limit)
		{
			distributed_send_to_machine_reliably(machine_index, &message, _distributed_message_object_changes, count,
				(word)(sizeof(message.header) + count * sizeof(struct distributed_object_change)));
			count = 0;
		}
	}
	if (count)
	{
		distributed_send_to_machine_reliably(machine_index, &message, _distributed_message_object_changes, count,
			(word)(sizeof(message.header) + count * sizeof(struct distributed_object_change)));
	}
	distributed_send_to_machine_reliably(machine_index, &message, _distributed_message_objects_synchronized, 0,
		(word)sizeof(message.header));
}

/* the host's object at an absolute index whose state goes to its clients
(not players' living units, which have their own messages, nor what is
attached or carried), NONE for none */
static long distributed_host_placed_object(
	long absolute_index)
{
	long object_index = objects_host_told[absolute_index];
	struct object_datum *object;

	if (object_index == NONE || !object_try_and_get(object_index))
		return NONE;
	object = object_get(object_index);
	if (object->object.parent_object_index != NONE || !TEST_FLAG(object->object.flags, _object_connected_to_map_bit))
		return NONE;
	/* (out of the map, where nobody sees it: a body fallen through the
	ground falls on for good, ever faster, never at rest) */
	if (TEST_FLAG(object->object.flags, _object_outside_of_map_bit))
		return NONE;
	if (TEST_FLAG(_object_mask_unit, object->object.type) && unit_get(object_index)->unit.player_index != NONE &&
		!TEST_FLAG(object->object.damage_flags, _object_dead_bit))
	{
		return NONE;
	}
	return object_index;
}

/* each client machine: where its players' living units are, and the
vehicles they drive, once a tick */
static void distributed_host_find_viewers(
	void)
{
	short machine_number;

	objects_host_viewers.count = distributed_client_machines(objects_host_viewers.indices,
		HALO_PORT_MAXIMUM_NETWORK_MACHINES);
	for (machine_number = 0; machine_number < objects_host_viewers.count; machine_number++)
	{
		long *player_list = machine_get_player_list(objects_host_viewers.indices[machine_number]);
		short local_player_index;

		objects_host_viewers.machines[machine_number].unit_count = 0;
		objects_host_viewers.machines[machine_number].vehicle_count = 0;
		for (local_player_index = 0; local_player_index < MAXIMUM_LOCAL_PLAYERS; local_player_index++)
		{
			struct player_datum *player = player_list[local_player_index] != NONE ?
				player_try_and_get(player_list[local_player_index]) : NULL;
			long unit_index = distributed_living_unit(player);
			long vehicle_index = unit_index != NONE ? distributed_driven_vehicle(player) : NONE;
			short count;

			if (unit_index == NONE)
				continue;
			count = objects_host_viewers.machines[machine_number].unit_count++;
			objects_host_viewers.machines[machine_number].unit_indices[count] = unit_index;
			object_get_origin(unit_index, &objects_host_viewers.machines[machine_number].origins[count]);
			if (vehicle_index != NONE)
			{
				count = objects_host_viewers.machines[machine_number].vehicle_count++;
				objects_host_viewers.machines[machine_number].vehicle_indices[count] = vehicle_index;
				objects_host_viewers.machines[machine_number].vehicle_player_indices[count] =
					(short)DATUM_INDEX_TO_ABSOLUTE_INDEX(player_list[local_player_index]);
			}
		}
	}
}

/* how often (ticks) the host sends that client machine an object where it
is, moving: by its nearest player; every tick to one with none in the
world (dead, it watches anyone) */
static short distributed_host_object_period(
	short machine_number,
	real_point3d const *position)
{
	real nearest = -1.0f;
	short index;

	for (index = 0; index < objects_host_viewers.machines[machine_number].unit_count; index++)
	{
		real_point3d const *origin = &objects_host_viewers.machines[machine_number].origins[index];
		real dx = position->x - origin->x;
		real dy = position->y - origin->y;
		real dz = position->z - origin->z;
		real distance_squared = dx * dx + dy * dy + dz * dz;

		if (nearest < 0.0f || distance_squared < nearest)
			nearest = distance_squared;
	}
	return nearest < NEAR_OBJECT_DISTANCE * NEAR_OBJECT_DISTANCE ? 1 :
		nearest < MIDDLE_OBJECT_DISTANCE * MIDDLE_OBJECT_DISTANCE ? 2 :
		nearest < FAR_OBJECT_DISTANCE * FAR_OBJECT_DISTANCE ? 3 : MAXIMUM_OBJECT_PERIOD_TICKS;
}

/* the kinds of states sent this tick */
enum
{
	/* moving: to each client as often as it is near its players */
	_host_state_moving,
	/* come to rest, or at rest in its turn: to every client */
	_host_state_to_all,
};

/* where the moving objects are, to each client those near its players
every tick, those further less often, and the vehicle it drives itself
(which it has already) every few ticks, with which of its ticks the host
has it at; to every client each as it comes to rest, and a few at rest,
round them all (one whose last move was lost is put right when its turn
comes) */
static void distributed_host_send_states(
	void)
{
	/* (one come to rest may be in its turn too) */
	static struct distributed_object_state states[MAXIMUM_TRACKED_OBJECTS + RESTING_STATES_PER_TICK];
	static byte kinds[MAXIMUM_TRACKED_OBJECTS + RESTING_STATES_PER_TICK];
	struct distributed_object_state_message message;
	short limit = MIN(MAXIMUM_ENTRIES_PER_MESSAGE, DATAGRAM_ENTRIES(struct distributed_object_state));
	short resting = 0;
	long told_count = objects_host_told_count;
	long state_count = 0;
	long absolute_index;
	long step;
	short machine_number;

	/* the moving ones, and those at rest now that were not */
	for (absolute_index = 0; absolute_index < told_count; absolute_index++)
	{
		long object_index = distributed_host_placed_object(absolute_index);
		boolean was_moving = objects_host_state_moving[absolute_index];
		boolean at_rest;

		objects_host_state_moving[absolute_index] = FALSE;
		if (object_index == NONE)
			continue;
		at_rest = TEST_FLAG(object_get(object_index)->object.flags, _object_at_rest_bit);
		objects_host_state_moving[absolute_index] = !at_rest;
		if (at_rest && !was_moving)
			continue;
		distributed_state_from_object(object_index, &states[state_count]);
		kinds[state_count++] = at_rest ? _host_state_to_all : _host_state_moving;
	}
	/* those at rest from the cursor on, round to it */
	for (step = 0; step < told_count && resting < RESTING_STATES_PER_TICK; step++)
	{
		long object_index;

		absolute_index = (objects_host_resting_cursor + step) % told_count;
		object_index = distributed_host_placed_object(absolute_index);
		if (object_index == NONE || !TEST_FLAG(object_get(object_index)->object.flags, _object_at_rest_bit))
			continue;
		resting++;
		objects_host_resting_cursor = (absolute_index + 1) % told_count;
		distributed_state_from_object(object_index, &states[state_count]);
		kinds[state_count++] = _host_state_to_all;
	}
	for (machine_number = 0; machine_number < objects_host_viewers.count; machine_number++)
	{
		long machine_index = objects_host_viewers.indices[machine_number];
		short count = 0;
		long index;

		for (index = 0; index < state_count; index++)
		{
			struct distributed_object_state *state = &message.states[count];
			short own_vehicle = NONE;
			short vehicle_number;

			for (vehicle_number = 0; vehicle_number < objects_host_viewers.machines[machine_number].vehicle_count;
				vehicle_number++)
			{
				if (objects_host_viewers.machines[machine_number].vehicle_indices[vehicle_number] ==
					states[index].object_index)
				{
					own_vehicle = vehicle_number;
				}
			}
			if (kinds[index] == _host_state_moving)
			{
				short period = own_vehicle != NONE ? OWN_VEHICLE_PERIOD_TICKS :
					distributed_host_object_period(machine_number, &states[index].position);

				if ((game_time_get() + (own_vehicle != NONE ? 0 : DATUM_INDEX_TO_ABSOLUTE_INDEX(states[index].object_index))) %
					period != 0)
				{
					continue;
				}
			}
			*state = states[index];
			/* (its own vehicle where it had it: at which of its ticks) */
			if (own_vehicle != NONE)
			{
				short player_index = objects_host_viewers.machines[machine_number].vehicle_player_indices[own_vehicle];

				/* (the client's clock against the host's, which a gap or a
				prediction refused since leaves as it was) */
				if (player_index >= 0 && player_index < MAXIMUM_TRACKED_PLAYERS &&
					objects_host_vehicle_predictions[player_index].accepted_vehicle_index == state->object_index &&
					game_time_get() - objects_host_vehicle_predictions[player_index].accepted_host_time <
						VEHICLE_PREDICTION_REFERENCE_TICKS)
				{
					SET_FLAG(state->flags, _distributed_object_predicted_bit, TRUE);
					state->time = (short)(word)(objects_host_vehicle_predictions[player_index].accepted_time +
						game_time_get() - objects_host_vehicle_predictions[player_index].accepted_host_time);
				}
			}
			if (++count == limit)
			{
				distributed_send_to_machine(machine_index, &message, _distributed_message_object_states, count,
					(word)(sizeof(message.header) + count * sizeof(struct distributed_object_state)));
				count = 0;
			}
		}
		if (count)
		{
			distributed_send_to_machine(machine_index, &message, _distributed_message_object_states, count,
				(word)(sizeof(message.header) + count * sizeof(struct distributed_object_state)));
		}
	}
}

static void distributed_inventory_from_unit(
	long unit_index,
	struct distributed_inventory *inventory)
{
	struct unit_datum *unit = unit_get(unit_index);
	short weapon_slot;

	csmemset(inventory, 0, sizeof(*inventory));
	inventory->unit_index = unit_index;
	csmemcpy(inventory->grenade_counts, unit->unit.grenade_counts, sizeof(inventory->grenade_counts));
	inventory->current_weapon_index = (char)unit->unit.current_weapon_index;
	for (weapon_slot = 0; weapon_slot < MAXIMUM_WEAPONS_PER_UNIT; weapon_slot++)
	{
		long weapon_index = unit->unit.weapon_object_indices[weapon_slot];
		struct weapon_datum *weapon = weapon_index != NONE ? weapon_try_and_get(weapon_index) : NULL;

		inventory->weapon_indices[weapon_slot] = NONE;
		if (weapon)
		{
			short magazine;

			inventory->weapon_indices[weapon_slot] = weapon_index;
			for (magazine = 0; magazine < 2; magazine++)
			{
				inventory->rounds_total[weapon_slot][magazine] = weapon->weapon.magazines[magazine].rounds_total;
				inventory->rounds_loaded[weapon_slot][magazine] = weapon->weapon.magazines[magazine].rounds_loaded;
			}
			{
				real age = weapon->weapon.age;

				/* (so written that one not a number is none) */
				age = age > 1.0f ? 1.0f : age >= 0.0f ? age : 0.0f;
				inventory->age[weapon_slot] = (word)(long)floor(age * AGE_SCALE + 0.5f);
			}
		}
	}
}

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

/* the kinds of inventories sent this time */
enum
{
	/* its weapons or grenades changed, or not sent for a while: to every
	client */
	_host_inventory_to_all,
	/* its ammunition alone changed: to its own client, and to the others as
	often as they are sent where it is */
	_host_inventory_ammunition,
	/* ... not now, but lately: to the others, in their turn */
	_host_inventory_ammunition_lately,
};

/* what every unit carries, when that has changed or not been sent for a
while: a change of weapons or grenades to every client at once, one of
ammunition alone to the unit's own client at once and to the others by how
near their players are, as the host sends them objects (those further off
see its shots less) */
static void distributed_host_send_inventories(
	void)
{
	static struct distributed_inventory inventories[MAXIMUM_TRACKED_OBJECTS];
	static byte kinds[MAXIMUM_TRACKED_OBJECTS];
	static real_point3d origins[MAXIMUM_TRACKED_OBJECTS];
	struct distributed_inventory_message message;
	short limit = MIN(MAXIMUM_ENTRIES_PER_MESSAGE, DATAGRAM_ENTRIES(struct distributed_inventory));
	long inventory_count = 0;
	long round = game_time_get() / INVENTORY_INTERVAL_TICKS;
	struct object_iterator iterator;
	short machine_number;

	object_iterator_new(&iterator, _object_mask_unit, 0);
	while (object_iterator_next(&iterator) && inventory_count < MAXIMUM_TRACKED_OBJECTS)
	{
		struct unit_datum *unit = unit_get(iterator.index);
		long absolute_index = DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.index);
		struct distributed_inventory *inventory = &inventories[inventory_count];
		short weapon_slot;
		boolean carries = unit->unit.grenade_counts[0] || unit->unit.grenade_counts[1];
		unsigned long checksum;
		unsigned long weapons_checksum;
		byte kind;

		for (weapon_slot = 0; weapon_slot < MAXIMUM_WEAPONS_PER_UNIT; weapon_slot++)
			carries |= unit->unit.weapon_object_indices[weapon_slot] != NONE;
		if (!distributed_object_networked(iterator.index) || TEST_FLAG(unit->object.damage_flags, _object_dead_bit))
			continue;
		/* (one carrying nothing, as it has come to for a while: its last
		weapon out of its hands everywhere, and then no more of it) */
		if (carries)
			objects_host_inventories[absolute_index].carried_time = game_time_get();
		else if (objects_host_inventories[absolute_index].carried_time == NONE ||
			game_time_get() - objects_host_inventories[absolute_index].carried_time >= EMPTY_INVENTORY_TICKS)
		{
			continue;
		}
		distributed_inventory_from_unit(iterator.index, inventory);
		checksum = distributed_checksum(inventory, sizeof(*inventory));
		/* (its weapons, the one in hand and its grenades: the bytes up to the
		ammunition) */
		weapons_checksum = distributed_checksum(inventory, (long)offsetof(struct distributed_inventory, rounds_total));
		if (objects_host_inventories[absolute_index].weapons_checksum != weapons_checksum ||
			game_time_get() - objects_host_inventories[absolute_index].time >= INVENTORY_REFRESH_TICKS)
		{
			kind = _host_inventory_to_all;
			objects_host_inventories[absolute_index].time = game_time_get();
			objects_host_inventories[absolute_index].ammunition_time = NONE;
		}
		else if (objects_host_inventories[absolute_index].checksum != checksum)
		{
			kind = _host_inventory_ammunition;
			objects_host_inventories[absolute_index].ammunition_time = game_time_get();
		}
		else if (objects_host_inventories[absolute_index].ammunition_time != NONE &&
			game_time_get() - objects_host_inventories[absolute_index].ammunition_time <
				MAXIMUM_OBJECT_PERIOD_TICKS * INVENTORY_INTERVAL_TICKS)
		{
			kind = _host_inventory_ammunition_lately;
		}
		else
		{
			continue;
		}
		objects_host_inventories[absolute_index].checksum = checksum;
		objects_host_inventories[absolute_index].weapons_checksum = weapons_checksum;
		kinds[inventory_count] = kind;
		object_get_origin(iterator.index, &origins[inventory_count]);
		inventory_count++;
	}
	for (machine_number = 0; machine_number < objects_host_viewers.count; machine_number++)
	{
		long machine_index = objects_host_viewers.indices[machine_number];
		short count = 0;
		long index;

		for (index = 0; index < inventory_count; index++)
		{
			if (kinds[index] != _host_inventory_to_all)
			{
				long absolute_index = DATUM_INDEX_TO_ABSOLUTE_INDEX(inventories[index].unit_index);
				boolean own = FALSE;
				short unit_number;

				for (unit_number = 0; unit_number < objects_host_viewers.machines[machine_number].unit_count; unit_number++)
					own |= objects_host_viewers.machines[machine_number].unit_indices[unit_number] == inventories[index].unit_index;
				if (own)
				{
					/* (its own unit's: each change at once) */
					if (kinds[index] != _host_inventory_ammunition)
						continue;
				}
				else
				{
					/* (another's: every round near its players, further every
					second to fourth, in the unit's turn, which comes once in
					the rounds after each change that its period spans) */
					short period = distributed_host_object_period(machine_number, &origins[index]);

					if ((round + absolute_index) % period != 0 ||
						game_time_get() - objects_host_inventories[absolute_index].ammunition_time >=
							period * INVENTORY_INTERVAL_TICKS)
					{
						continue;
					}
				}
			}
			message.inventories[count] = inventories[index];
			if (++count == limit)
			{
				distributed_send_to_machine(machine_index, &message, _distributed_message_inventories, count,
					(word)(sizeof(message.header) + count * sizeof(struct distributed_inventory)));
				count = 0;
			}
		}
		if (count)
		{
			distributed_send_to_machine(machine_index, &message, _distributed_message_inventories, count,
				(word)(sizeof(message.header) + count * sizeof(struct distributed_inventory)));
		}
	}
}

/* (the host) the vehicles a client's own players drive: the latest of
each, taken at the next tick */
void network_objects_handle_vehicle_prediction(
	long machine_index,
	void const *entries,
	short count)
{
	struct distributed_object_state const *states = (struct distributed_object_state const *)entries;
	short index;

	for (index = 0; index < count; index++)
	{
		struct distributed_object_state const *state = &states[index];
		struct unit_datum *vehicle = distributed_object_index_valid(state->object_index) ?
			(struct unit_datum *)object_try_and_get_and_verify_type(state->object_index, _object_mask_vehicle) : NULL;
		short player_index;

		if (!vehicle || vehicle->unit.driver_object_index == NONE)
			continue;
		if (unit_get(vehicle->unit.driver_object_index)->unit.player_index == NONE)
			continue;
		player_index = (short)DATUM_INDEX_TO_ABSOLUTE_INDEX(unit_get(vehicle->unit.driver_object_index)->unit.player_index);
		if (player_index < 0 || player_index >= MAXIMUM_TRACKED_PLAYERS ||
			!distributed_machine_has_player(machine_index, player_index))
		{
			continue;
		}
		objects_host_vehicle_predictions[player_index].valid = TRUE;
		objects_host_vehicle_predictions[player_index].machine_index = machine_index;
		objects_host_vehicle_predictions[player_index].state = *state;
	}
}

/* (the host) where a client's player's vehicle, which that player drives,
left the ground (network_objects_vehicle_below_ceiling): noted each tick,
from the host's copy */
static void network_objects_note_vehicle_ground(
	short player_index)
{
	long vehicle_index = objects_host_vehicle_predictions[player_index].accepted_vehicle_index;
	struct vehicle_datum *vehicle = vehicle_index != NONE ?
		(struct vehicle_datum *)object_try_and_get_and_verify_type(vehicle_index, _object_mask_vehicle) : NULL;
	struct unit_datum *driver;

	if (!vehicle || vehicle->unit.driver_object_index == NONE ||
		!(driver = unit_get(vehicle->unit.driver_object_index)) || driver->unit.player_index == NONE ||
		DATUM_INDEX_TO_ABSOLUTE_INDEX(driver->unit.player_index) != player_index)
	{
		objects_host_vehicle_predictions[player_index].ground_vehicle_index = NONE;
		return;
	}
	/* (on the ground, or one new to it: from where it is) */
	if (vehicle->vehicle.airborne_ticks == 0 ||
		objects_host_vehicle_predictions[player_index].ground_vehicle_index != vehicle_index)
	{
		objects_host_vehicle_predictions[player_index].ground_vehicle_index = vehicle_index;
		objects_host_vehicle_predictions[player_index].ground_time = game_time_get();
		objects_host_vehicle_predictions[player_index].ground_height = vehicle->object.position.z;
		objects_host_vehicle_predictions[player_index].ground_rise_speed =
			MAX(vehicle->object.translational_velocity.k, 0.0f);
		/* (so written that a speed not a number is none) */
		if (!(objects_host_vehicle_predictions[player_index].ground_rise_speed <= MAXIMUM_PREDICTED_VEHICLE_SPEED))
			objects_host_vehicle_predictions[player_index].ground_rise_speed = 0.0f;
	}
}

/* (the host) whether a client's player's vehicle that does not fly (nor
floats) may be where they say it is: on the ground anywhere its speed
takes it; in the air no higher above where it left the ground than a thing
thrown up as fast as it went up then (and a tolerance) falls to since (the
ticks since, less the client's round trip, no more than
VEHICLE_PREDICTION_CEILING_LEAD_TICKS, and jitter, which are the client's
ahead): no flying, nor hovering */
static boolean network_objects_vehicle_below_ceiling(
	short player_index,
	long vehicle_index,
	real_point3d const *position)
{
	struct vehicle_datum *vehicle = vehicle_datum_get(vehicle_index);
	short type = vehicle_specific_definition_get(vehicle->definition_index)->vehicle_type;
	real ticks;
	real ceiling;

	if (type == _vehicle_type_human_boat || type == _vehicle_type_human_plane ||
		type == _vehicle_type_alien_fighter || type == _vehicle_type_turret ||
		objects_host_vehicle_predictions[player_index].ground_vehicle_index != vehicle_index ||
		vehicle->vehicle.airborne_ticks == 0)
	{
		return TRUE;
	}
	ticks = (real)(game_time_get() - objects_host_vehicle_predictions[player_index].ground_time) -
		MIN(distributed_machine_round_trip_ticks(objects_host_vehicle_predictions[player_index].machine_index),
			(real)VEHICLE_PREDICTION_CEILING_LEAD_TICKS) -
		(real)VEHICLE_PREDICTION_JITTER_TICKS;
	if (!(ticks > 0.0f) || !(global_gravity > 0.0f))
		return TRUE;
	ceiling = VEHICLE_PREDICTION_RISE_TOLERANCE +
		(objects_host_vehicle_predictions[player_index].ground_rise_speed + PREDICTED_VEHICLE_SPEED_MARGIN) * ticks -
		0.5f * global_gravity * ticks * ticks;
	/* (so written that a position not a number is not below) */
	return position->z - objects_host_vehicle_predictions[player_index].ground_height <= ceiling;
}

/* ... taken as they are, within a tolerance, if that machine's player
still drives it: a little off closed by half, more put there. Each taken
moves the host's copy, and the next is measured from there, so a client
could take its vehicle HOST_VEHICLE_ACCEPT_TOLERANCE further each tick: a
prediction is also no further from the anchor (one taken, a newer once a
second) than the vehicle goes in the client's ticks since (after it, and no
more than the host's since and a little jitter), with a margin, and no
faster: as fast as twice its tag's top speed, or as the host's own ticks
sent its copy lately (thrown), whichever is faster. What they sent it is
its speed less as much as the velocity it took of the client was faster
than twice the top speed: a client that says it goes faster gains nothing
by it (its gravity on a copy said to hover and fall). A teleporter moves
the host's copy too, further than it goes since the last taken: it is
measured from where the host has it only, a new anchor. In the air it is
no higher than network_objects_vehicle_below_ceiling says. Which of the
client's ticks the host has it at is noted, to tell the client. */
void network_objects_apply_vehicle_predictions(
	void)
{
	long now = game_time_get();
	short player_index;

	for (player_index = 0; player_index < MAXIMUM_TRACKED_PLAYERS; player_index++)
	{
		struct distributed_object_state const *state = &objects_host_vehicle_predictions[player_index].state;
		struct unit_datum *vehicle;
		struct unit_datum *driver;
		real_vector3d forward, up, velocity, angular_velocity;
		real dx, dy, dz;
		real speed;
		boolean anchored;

		network_objects_note_vehicle_ground(player_index);
		if (!objects_host_vehicle_predictions[player_index].valid)
			continue;
		objects_host_vehicle_predictions[player_index].valid = FALSE;
		vehicle = (struct unit_datum *)object_try_and_get_and_verify_type(state->object_index, _object_mask_vehicle);
		if (!vehicle || vehicle->unit.driver_object_index == NONE || vehicle->object.parent_object_index != NONE)
			continue;
		driver = unit_get(vehicle->unit.driver_object_index);
		if (driver->unit.player_index == NONE ||
			DATUM_INDEX_TO_ABSOLUTE_INDEX(driver->unit.player_index) != player_index ||
			!distributed_machine_has_player(objects_host_vehicle_predictions[player_index].machine_index, player_index))
		{
			continue;
		}
		dx = state->position.x - vehicle->object.position.x;
		dy = state->position.y - vehicle->object.position.y;
		dz = state->position.z - vehicle->object.position.z;
		/* (so written that a position not a number is not taken) */
		if (!(dx * dx + dy * dy + dz * dz <= HOST_VEHICLE_ACCEPT_TOLERANCE * HOST_VEHICLE_ACCEPT_TOLERANCE))
			continue;
		/* (no higher in the air than it was thrown up leaving the ground,
		falling since: past that the host's copy falls as its ticks have it) */
		if (!network_objects_vehicle_below_ceiling(player_index, state->object_index, &state->position))
			continue;
		/* (how fast it may go, a tick) */
		{
			struct vehicle_definition const *definition = vehicle_specific_definition_get(vehicle->definition_index);
			real top_speed = 2.0f * MAX(definition->maximum_forward_speed, definition->maximum_reverse_speed);
			real_vector3d const *host_velocity = &vehicle->object.translational_velocity;
			real host_speed = (real)sqrt(host_velocity->i * host_velocity->i + host_velocity->j * host_velocity->j +
				host_velocity->k * host_velocity->k);
			long *speed_time = &objects_host_vehicle_predictions[player_index].speed_time;
			real *speeds = objects_host_vehicle_predictions[player_index].speeds;

			/* (so written that a tag's speed not a number is none) */
			if (!(top_speed >= 0.0f))
				top_speed = 0.0f;
			/* (what the host's ticks sent it since the last taken) */
			if (objects_host_vehicle_predictions[player_index].accepted_vehicle_index == state->object_index)
			{
				real_vector3d const *taken = &objects_host_vehicle_predictions[player_index].accepted_velocity;
				real taken_speed = (real)sqrt(taken->i * taken->i + taken->j * taken->j + taken->k * taken->k);

				if (taken_speed > top_speed)
					host_speed -= taken_speed - top_speed;
			}
			/* (the most of each span; so written that a speed not a number
			is none) */
			if (now < *speed_time || now - *speed_time >= 2 * VEHICLE_PREDICTION_SPEED_TICKS)
			{
				*speed_time = now;
				speeds[0] = 0.0f;
				speeds[1] = 0.0f;
			}
			else if (now - *speed_time >= VEHICLE_PREDICTION_SPEED_TICKS)
			{
				*speed_time = now;
				speeds[1] = speeds[0];
				speeds[0] = 0.0f;
			}
			if (host_speed > speeds[0])
				speeds[0] = host_speed;
			speed = MAX(top_speed, MAX(speeds[0], speeds[1]));
			speed = MIN(speed, MAXIMUM_PREDICTED_VEHICLE_SPEED);
		}
		anchored = objects_host_vehicle_predictions[player_index].accepted_vehicle_index == state->object_index &&
			now - objects_host_vehicle_predictions[player_index].anchor_host_time < VEHICLE_PREDICTION_REFERENCE_TICKS;
		if (anchored)
		{
			long host_ticks = now - objects_host_vehicle_predictions[player_index].accepted_host_time;
			real reach = (speed + PREDICTED_VEHICLE_SPEED_MARGIN) * (real)host_ticks + HOST_VEHICLE_BLEND_DISTANCE;
			real_point3d const *from = &objects_host_vehicle_predictions[player_index].accepted_host_position;

			dx = vehicle->object.position.x - from->x;
			dy = vehicle->object.position.y - from->y;
			dz = vehicle->object.position.z - from->z;
			/* (the host's copy moved further than it goes since the last
			taken, which only the host moves it by: taken from where it is,
			a new anchor) */
			if (!(dx * dx + dy * dy + dz * dz <= reach * reach))
			{
				anchored = FALSE;
			}
			else
			{
				/* (a client's clock that jumped counts no more than the
				host's ticks since and the jitter) */
				long ticks = MIN((long)(short)(word)((word)state->time - objects_host_vehicle_predictions[player_index].anchor_time),
					now - objects_host_vehicle_predictions[player_index].anchor_host_time + VEHICLE_PREDICTION_JITTER_TICKS);

				reach = (speed + PREDICTED_VEHICLE_SPEED_MARGIN) * (real)ticks + HOST_VEHICLE_BLEND_DISTANCE;
				from = &objects_host_vehicle_predictions[player_index].anchor_position;
				dx = state->position.x - from->x;
				dy = state->position.y - from->y;
				dz = state->position.z - from->z;
				if (ticks <= 0 || !(dx * dx + dy * dy + dz * dz <= reach * reach))
					continue;
			}
		}
		distributed_object_state_unpack(state, &forward, &up, &velocity, &angular_velocity);
		/* (no faster than it goes: the margin is for where it is only, else
		its copy would go faster by it each tick) */
		distributed_vector_clamp(&velocity, speed);
		distributed_vector_clamp(&angular_velocity, MAXIMUM_PREDICTED_VEHICLE_ANGULAR_SPEED);
		if (!distributed_transform_valid(&state->position, &forward, &up, &velocity, &angular_velocity, &forward, &up))
			continue;
		{
			real_point3d previous_position = vehicle->object.position;

			network_objects_reconcile(state->object_index, &state->position, &forward, &up, &velocity,
				&angular_velocity, HOST_VEHICLE_BLEND_DISTANCE);
			/* (a copy at rest runs no physics, and so neither falls nor
			counts its ticks in the air: one the client moved from where it
			came to rest (a little at a time too), or says moves, wakes, and
			settles again if it is still) */
			if (!TEST_FLAG(vehicle->object.flags, _object_at_rest_bit))
			{
				objects_host_vehicle_predictions[player_index].rest_vehicle_index = NONE;
			}
			else
			{
				if (objects_host_vehicle_predictions[player_index].rest_vehicle_index != state->object_index)
				{
					objects_host_vehicle_predictions[player_index].rest_vehicle_index = state->object_index;
					objects_host_vehicle_predictions[player_index].rest_position = previous_position;
				}
				dx = vehicle->object.position.x - objects_host_vehicle_predictions[player_index].rest_position.x;
				dy = vehicle->object.position.y - objects_host_vehicle_predictions[player_index].rest_position.y;
				dz = vehicle->object.position.z - objects_host_vehicle_predictions[player_index].rest_position.z;
				if (!(dx * dx + dy * dy + dz * dz <=
						VEHICLE_PREDICTION_STILL_DISTANCE * VEHICLE_PREDICTION_STILL_DISTANCE) ||
					velocity.i != 0.0f || velocity.j != 0.0f || velocity.k != 0.0f)
				{
					SET_FLAG(vehicle->object.flags, _object_at_rest_bit, FALSE);
					objects_host_vehicle_predictions[player_index].rest_vehicle_index = NONE;
				}
			}
		}
		objects_host_vehicle_predictions[player_index].accepted_vehicle_index = state->object_index;
		objects_host_vehicle_predictions[player_index].accepted_time = (word)state->time;
		objects_host_vehicle_predictions[player_index].accepted_host_time = now;
		objects_host_vehicle_predictions[player_index].accepted_host_position = vehicle->object.position;
		objects_host_vehicle_predictions[player_index].accepted_velocity = vehicle->object.translational_velocity;
		/* (a new anchor: the first, one a second on, or after a jump) */
		if (!anchored ||
			now - objects_host_vehicle_predictions[player_index].anchor_host_time >= VEHICLE_PREDICTION_ANCHOR_TICKS)
		{
			objects_host_vehicle_predictions[player_index].anchor_time = (word)state->time;
			objects_host_vehicle_predictions[player_index].anchor_host_time = now;
			objects_host_vehicle_predictions[player_index].anchor_position = state->position;
		}
	}
}

void network_objects_host_tick(
	void)
{
	distributed_host_update_objects();
	distributed_host_find_viewers();
	distributed_host_send_states();
	if (game_time_get() % INVENTORY_INTERVAL_TICKS == 0)
		distributed_host_send_inventories();
}

/* ---------- a client */

/* the item out of whatever unit has it */
static void distributed_client_release_item(
	long item_index,
	boolean deleting)
{
	struct object_iterator iterator;

	object_iterator_new(&iterator, _object_mask_unit, 0);
	while (object_iterator_next(&iterator))
	{
		struct unit_datum *unit = unit_get(iterator.index);
		short weapon_slot;

		for (weapon_slot = 0; weapon_slot < MAXIMUM_WEAPONS_PER_UNIT; weapon_slot++)
		{
			if (unit->unit.weapon_object_indices[weapon_slot] != item_index)
				continue;
			if (deleting)
				unit_network_forget_weapon(iterator.index, weapon_slot);
			else
				unit_network_drop_weapon(iterator.index, weapon_slot);
		}
	}
}

/* deletes an object (the host's, on its word, or one the host has not
told of), undoing what refers to it first */
static void distributed_client_delete(
	long object_index)
{
	struct object_datum *object = object_try_and_get(object_index);

	if (!object)
		return;
	if (TEST_FLAG(_object_mask_item, object->object.type))
		distributed_client_release_item(object_index, TRUE);
	if (TEST_FLAG(_object_mask_unit, object->object.type))
	{
		struct unit_datum *unit = unit_get(object_index);
		long child_index;
		short weapon_slot;

		/* (the host's weapons it carries out: deleted with it otherwise, one
		the host's unit had let go of before (its inventory not here yet)
		would be gone here for good; the host's word deletes those it deleted
		with its unit) */
		for (weapon_slot = 0; weapon_slot < MAXIMUM_WEAPONS_PER_UNIT; weapon_slot++)
		{
			if (unit->unit.weapon_object_indices[weapon_slot] != NONE &&
				network_objects_client_has(unit->unit.weapon_object_indices[weapon_slot]))
			{
				unit_network_drop_weapon(object_index, weapon_slot);
			}
		}
		child_index = unit->object.first_child_object_index;
		/* (its riders out first: they are deleted with it otherwise) */
		while (child_index != NONE)
		{
			long next_index = object_get(child_index)->object.next_object_index;

			if (TEST_FLAG(_object_mask_unit, object_get(child_index)->object.type) &&
				unit_get(child_index)->unit.parent_seat_index != NONE)
			{
				unit_exit_seat_end(child_index);
			}
			child_index = next_index;
		}
		if (unit->unit.player_index != NONE && player_try_and_get(unit->unit.player_index) &&
			player_get(unit->unit.player_index)->unit_index == object_index)
		{
			network_player_detach_unit(unit->unit.player_index);
		}
	}
	/* (a player's action on it, the prompt the hud draws: deleted between
	ticks, it would draw on with the object gone until the next tick looked
	again) */
	{
		struct data_iterator iterator;
		struct player_datum *player;

		data_iterator_new(&iterator, player_data);
		while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
		{
			if (player->action_object_index == object_index)
			{
				player->action_result = _player_action_result_reload;
				player->action_object_index = NONE;
			}
		}
	}
	objects_client_deleting = TRUE;
	object_delete_immediately(object_index);
	objects_client_deleting = FALSE;
}

/* a create failed (the object pool full, its place not to be had): all of
them again from the host, a while after (asked again, flagged: the host
tells them all only then, not on each asking while they are on their way),
longer each time it fails again */
static void distributed_client_create_failed(
	void)
{
	objects_statistics.create_failures++;
	/* (not asked since the last: that asking covers this one) */
	if (objects_client_failed_time != NONE && objects_client_ready_time <= objects_client_failed_time)
		return;
	objects_client_ask_again = TRUE;
	objects_client_failed_time = game_time_get();
	objects_client_synchronized = FALSE;
	objects_client_ready_time = game_time_get();
	objects_client_ready_interval = objects_client_retry_ticks;
	objects_client_retry_ticks = MIN(2 * objects_client_retry_ticks, CLIENT_RETRY_MAXIMUM_TICKS);
}

/* whether the object is this machine's own player's unit or the vehicle it
drives (where they are is its own) */
static boolean distributed_client_own_object(
	long object_index)
{
	struct object_datum *object = object_get(object_index);
	long unit_index = object_index;

	if (!TEST_FLAG(_object_mask_unit, object->object.type))
		return FALSE;
	if (object->object.type == _object_type_vehicle)
		unit_index = unit_get(object_index)->unit.driver_object_index;
	return unit_index != NONE && distributed_player_is_local(unit_get(unit_index)->unit.player_index);
}

/* whether the host's object can be made: of the kinds the host has, its
definition a tag of the group its type says, at a whole index, of a team
the game has, where the world is (its axes made a rotation in forward, up) */
static boolean distributed_client_change_valid(
	struct distributed_object_change const *change,
	real_vector3d *forward,
	real_vector3d *up)
{
	long group_tag;
	short type;
	short team_count = MAXIMUM_TRACKED_PLAYERS;

	if (!distributed_object_index_valid(change->object_index) ||
		!tag_index_is_group(change->definition_index, OBJECT_DEFINITION_TAG))
	{
		return FALSE;
	}
	type = object_definition_get(change->definition_index)->object.type;
	switch (type)
	{
	case _object_type_biped: group_tag = BIPED_DEFINITION_TAG; break;
	case _object_type_vehicle: group_tag = VEHICLE_DEFINITION_TAG; break;
	case _object_type_weapon: group_tag = WEAPON_DEFINITION_TAG; break;
	case _object_type_equipment: group_tag = EQUIPMENT_DEFINITION_TAG; break;
	default: return FALSE;
	}
	if (!TEST_FLAG(NETWORKED_OBJECT_TYPES, type) || !tag_index_is_group(change->definition_index, group_tag))
		return FALSE;
	/* (a flag's or ball's team names the game type's flag or ball: an index
	of its arrays; weapon_is_flag's bit) */
	if (type == _object_type_weapon && ((weapon_definition_get(change->definition_index)->weapon.flags >> 3) & 1))
		team_count = game_engine_get_variant()->game_engine_index == game_engine_ctf ? CTF_FLAG_TEAMS : ODDBALL_TEAMS;
	if (change->owner_team_index != NONE && (change->owner_team_index < 0 || change->owner_team_index >= team_count))
		return FALSE;
	/* (its colors numbers) */
	{
		short color_index;

		for (color_index = 0; color_index < NUMBER_OF_OBJECT_CHANGE_COLORS; color_index++)
		{
			if (!distributed_real_valid(change->change_colors[color_index].red) ||
				!distributed_real_valid(change->change_colors[color_index].green) ||
				!distributed_real_valid(change->change_colors[color_index].blue))
			{
				return FALSE;
			}
		}
	}
	return distributed_transform_valid(&change->position, &change->forward, &change->up,
		&change->translational_velocity, &change->angular_velocity, forward, up);
}

/* the host's word on an object made or found here: how it looks, whether
carried, whether dead */
static void distributed_client_apply_change(
	long object_index,
	struct distributed_object_change const *change)
{
	struct object_datum *object = object_get(object_index);

	csmemcpy(object->object.base_change_colors, change->change_colors, sizeof(object->object.base_change_colors));
	/* (a permutation the model has, or none: the model's renderer takes it
as it is) */
	{
		long model_index = object_definition_get(object->definition_index)->object.model.index;
		struct model *model = model_index != NONE ? model_definition_get(model_index) : NULL;
		short region_index;

		for (region_index = 0; model && region_index < model->regions.count &&
			region_index < MAXIMUM_REGIONS_PER_OBJECT; region_index++)
		{
			struct model_region *region = TAG_BLOCK_GET_ELEMENT(&model->regions, region_index, struct model_region);
			byte permutation_index = change->region_permutations[region_index];

			if (permutation_index == (byte)NONE || permutation_index < region->permutations.count)
				object->object.region_permutations[region_index] = permutation_index;
		}
	}
	object_set_garbage(object_index, FALSE);
	/* (in a unit's inventory: the next inventories put it there) */
	if (TEST_FLAG(change->flags, _distributed_object_carried_bit) &&
		TEST_FLAG(object->object.flags, _object_connected_to_map_bit) &&
		object->object.parent_object_index == NONE)
	{
		object_disconnect_from_map(object_index);
		object_set_visibility(object_index, FALSE);
	}
	/* (a body: dead on the host before this machine had it, killed here
	with nothing to show or count of it) */
	if (TEST_FLAG(change->flags, _distributed_object_dead_bit) && TEST_FLAG(_object_mask_unit, object->object.type) &&
		!TEST_FLAG(object->object.damage_flags, _object_dead_bit))
	{
		unit_kill_silent(object_index);
		unit_kill_no_statistics(object_index);
		/* (at once: a body where no player is may not be updated for long) */
		object_damage_update(object_index);
	}
}

static void distributed_client_create(
	struct distributed_object_change const *change)
{
	long absolute_index = DATUM_INDEX_TO_ABSOLUTE_INDEX(change->object_index);
	struct datum_header const *header;
	struct object_placement_data placement;
	real_vector3d forward, up;
	long object_index;

	if (absolute_index < 0 || absolute_index >= MAXIMUM_TRACKED_OBJECTS ||
		absolute_index >= object_header_data->maximum_count ||
		!distributed_client_change_valid(change, &forward, &up))
	{
		objects_statistics.create_failures++;
		return;
	}
	header = (struct datum_header const *)((byte const *)object_header_data->data +
		absolute_index * object_header_data->size);
	if (header->identifier)
	{
		long existing_index = ((long)header->identifier << 16) | absolute_index;
		struct object_datum *existing = object_try_and_get(existing_index);

		/* placed here when the map loaded, as on the host, or told of
		before: the same object, where the host has it (unless carried, or
		this machine's own) */
		if (existing_index == change->object_index && existing &&
			existing->definition_index == change->definition_index)
		{
			objects_client_has[absolute_index] = existing_index;
			if (!TEST_FLAG(change->flags, _distributed_object_carried_bit) &&
				existing->object.parent_object_index == NONE &&
				TEST_FLAG(existing->object.flags, _object_connected_to_map_bit) &&
				!distributed_client_own_object(existing_index))
			{
				distributed_object_move(existing_index, &change->position, &forward, &up,
					&change->translational_velocity, &change->angular_velocity);
			}
			distributed_client_apply_change(existing_index, change);
			return;
		}
		/* something else in its place: gone */
		distributed_client_delete(existing_index);
		objects_client_has[absolute_index] = NONE;
		if (header->identifier)
		{
			distributed_client_create_failed();
			return;
		}
	}
	object_placement_data_new(&placement, change->definition_index, NONE);
	placement.owner_player_index = distributed_player_from_byte(change->owner_player_index);
	placement.owner_team_index = change->owner_team_index;
	placement.variant_number = change->variant_number;
	placement.position = change->position;
	placement.forward = forward;
	placement.up = up;
	placement.translational_velocity = change->translational_velocity;
	placement.angular_velocity = change->angular_velocity;
	csmemcpy(placement.change_colors, change->change_colors, sizeof(placement.change_colors));
	objects_client_creating_index = change->object_index;
	objects_client_creating = TRUE;
	object_index = object_new(&placement);
	objects_client_creating = FALSE;
	objects_client_creating_index = NONE;
	objects_statistics.creates++;
	if (object_index != change->object_index)
	{
		if (object_index != NONE)
		{
			objects_client_deleting = TRUE;
			object_delete_immediately(object_index);
			objects_client_deleting = FALSE;
		}
		distributed_client_create_failed();
		return;
	}
	objects_client_has[absolute_index] = object_index;
	distributed_client_apply_change(object_index, change);
}

void network_objects_handle_changes(
	void const *entries,
	short count)
{
	struct distributed_object_change const *changes = (struct distributed_object_change const *)entries;
	short index;

	for (index = 0; index < count; index++)
	{
		struct distributed_object_change const *change = &changes[index];
		long absolute_index = DATUM_INDEX_TO_ABSOLUTE_INDEX(change->object_index);

		if (!distributed_object_index_valid(change->object_index) ||
			absolute_index < 0 || absolute_index >= MAXIMUM_TRACKED_OBJECTS)
		{
			continue;
		}
		if (change->change == _object_change_create)
		{
			distributed_client_create(change);
			if (objects_client_resynchronizing && objects_client_has[absolute_index] == change->object_index)
				objects_client_resync_seen[absolute_index] = TRUE;
		}
		else if (change->change == _object_change_delete &&
			objects_client_has[absolute_index] == change->object_index)
		{
			objects_statistics.deletes++;
			distributed_client_delete(change->object_index);
			objects_client_has[absolute_index] = NONE;
		}
	}
}

void network_objects_handle_synchronized(
	void)
{
	long absolute_index;
	/* (a create failed since this machine last asked: not all of them, it
	asks again) */
	if (objects_client_failed_time != NONE)
	{
		if (objects_client_ready_time <= objects_client_failed_time)
			return;
		objects_client_failed_time = NONE;
		objects_client_retry_ticks = CLIENT_RETRY_TICKS;
	}
	if (objects_client_resynchronizing)
	{
		for (absolute_index = 0; absolute_index < MAXIMUM_TRACKED_OBJECTS; absolute_index++)
		{
			if (objects_client_has[absolute_index] == NONE || objects_client_resync_seen[absolute_index])
				continue;
			distributed_client_delete(objects_client_has[absolute_index]);
			objects_client_has[absolute_index] = NONE;
		}
		objects_client_resynchronizing = FALSE;
	}
	objects_client_synchronized = TRUE;
}

/* a vehicle this machine's own player drives where the host has it, the
host saying at which of this machine's ticks (its prediction come back):
moved by how far that is from where this machine had it at that tick, if
further than a tolerance, keeping what it has done since (no rubber band a
round trip long); FALSE without that tick */
static boolean distributed_client_correct_own_vehicle(
	long vehicle_index,
	long player_index,
	struct distributed_object_state const *state)
{
	struct player_datum *player = player_try_and_get(player_index);
	short local_player_index = player ? player->local_player_index : NONE;
	long now = game_time_get();
	long time = now - (long)(word)((word)now - (word)state->time);
	struct object_datum *object = object_get(vehicle_index);
	real_vector3d error;
	real_vector3d forward, up;
	real_point3d before, position;
	short index;

	if (!TEST_FLAG(state->flags, _distributed_object_predicted_bit) ||
		local_player_index < 0 || local_player_index >= MAXIMUM_LOCAL_PLAYERS ||
		objects_client_own_vehicles[local_player_index][time & (OWN_VEHICLE_POSITION_TICKS - 1)].time != time ||
		objects_client_own_vehicles[local_player_index][time & (OWN_VEHICLE_POSITION_TICKS - 1)].vehicle_index !=
			vehicle_index)
	{
		return FALSE;
	}
	{
		struct distributed_own_vehicle const *own =
			&objects_client_own_vehicles[local_player_index][time & (OWN_VEHICLE_POSITION_TICKS - 1)];

		error.i = state->position.x - own->position.x;
		error.j = state->position.y - own->position.y;
		error.k = state->position.z - own->position.z;
	}
	if (error.i * error.i + error.j * error.j + error.k * error.k <= LOCAL_VEHICLE_TOLERANCE * LOCAL_VEHICLE_TOLERANCE)
		return TRUE;
	/* (turned and going as it drives it) */
	before = object->object.position;
	position.x = before.x + error.i;
	position.y = before.y + error.j;
	position.z = before.z + error.k;
	forward = object->object.forward;
	up = object->object.up;
	network_objects_correct(vehicle_index, &position, &forward, &up, NULL, NULL);
	distributed_count_correction();
	/* (the ticks noted since moved as it was, not corrected again) */
	error.i = object->object.position.x - before.x;
	error.j = object->object.position.y - before.y;
	error.k = object->object.position.z - before.z;
	for (index = 0; index < OWN_VEHICLE_POSITION_TICKS; index++)
	{
		if (objects_client_own_vehicles[local_player_index][index].vehicle_index == vehicle_index)
		{
			objects_client_own_vehicles[local_player_index][index].position.x += error.i;
			objects_client_own_vehicles[local_player_index][index].position.y += error.j;
			objects_client_own_vehicles[local_player_index][index].position.z += error.k;
		}
	}
	return TRUE;
}

/* (a client) a biped, no player's (a player's the units' states kill), the
host says is dead, alive here: its killing blow, unreliable, kills it; lost,
nothing else would. Killed with nothing to show or count of it once the host
has said so for a while. */
static void distributed_client_dead_biped(
	long object_index)
{
	long absolute_index = DATUM_INDEX_TO_ABSOLUTE_INDEX(object_index);
	struct unit_datum *unit = (struct unit_datum *)object_try_and_get_and_verify_type(object_index,
		_object_mask_biped);

	if (!unit || TEST_FLAG(unit->object.damage_flags, _object_dead_bit) || unit->unit.player_index != NONE)
		return;
	if (objects_client_dead[absolute_index].object_index != object_index)
	{
		objects_client_dead[absolute_index].object_index = object_index;
		objects_client_dead[absolute_index].time = game_time_get();
	}
	else if (game_time_get() - objects_client_dead[absolute_index].time >= DEAD_BIPED_FALLBACK_TICKS)
	{
		unit_kill_silent(object_index);
		unit_kill_no_statistics(object_index);
		object_damage_update(object_index);
	}
}

void network_objects_handle_states(
	void const *entries,
	short count)
{
	struct distributed_object_state const *states = (struct distributed_object_state const *)entries;
	short index;

	for (index = 0; index < count; index++)
	{
		struct distributed_object_state const *state = &states[index];
		struct object_datum *object;
		real tolerance = REMOTE_OBJECT_TOLERANCE;
		real angle_tolerance = REMOTE_OBJECT_ANGLE_TOLERANCE;
		real blend_distance = REMOTE_BLEND_DISTANCE;
		real_vector3d forward, up, velocity, angular_velocity;
		real dx, dy, dz;

		if (!distributed_object_index_valid(state->object_index) || !network_objects_client_has(state->object_index))
			continue;
		object = object_get(state->object_index);
		if (TEST_FLAG(state->flags, _distributed_object_dead_bit))
		{
			distributed_client_dead_biped(state->object_index);
			/* (a body its tag destroys at once is gone) */
			if (!object_try_and_get_and_verify_type(state->object_index, _object_mask_all))
				continue;
		}
		/* (an item made carried whose unit never had it here, placed on the
		host: in the world, seen) */
		if (TEST_FLAG(_object_mask_item, object->object.type) && object->object.parent_object_index == NONE &&
			!TEST_FLAG(object->object.flags, _object_connected_to_map_bit) &&
			!TEST_FLAG(item_get(state->object_index)->item.flags, _item_attached_to_unit_bit))
		{
			object_reconnect_to_map(state->object_index, NULL);
			object_set_visibility(state->object_index, TRUE);
		}
		if (object->object.parent_object_index != NONE ||
			!TEST_FLAG(object->object.flags, _object_connected_to_map_bit))
		{
			continue;
		}
		distributed_object_state_unpack(state, &forward, &up, &velocity, &angular_velocity);
		if (!distributed_transform_valid(&state->position, &forward, &up, NULL, NULL, &forward, &up))
			continue;
		/* (come to rest: all of the way, drawn gliding, not half of it and
		left there) */
		if (TEST_FLAG(state->flags, _distributed_object_at_rest_bit))
			blend_distance = 0.0f;
		/* (a vehicle this machine's own player drives is its own) */
		if (object->object.type == _object_type_vehicle)
		{
			struct unit_datum *vehicle = unit_get(state->object_index);

			if (blend_distance > 0.0f)
				blend_distance = REMOTE_VEHICLE_BLEND_DISTANCE;
			if (vehicle->unit.driver_object_index != NONE &&
				distributed_player_is_local(unit_get(vehicle->unit.driver_object_index)->unit.player_index))
			{
				if (distributed_client_correct_own_vehicle(state->object_index,
					unit_get(vehicle->unit.driver_object_index)->unit.player_index, state))
				{
					continue;
				}
				tolerance = LOCAL_VEHICLE_TOLERANCE;
				/* (turned as it drives it) */
				angle_tolerance = -1.0f;
				blend_distance = 0.0f;
			}
		}
		dx = state->position.x - object->object.position.x;
		dy = state->position.y - object->object.position.y;
		dz = state->position.z - object->object.position.z;
		/* (close enough where it is, and turned as it is: one at rest too) */
		if (dx * dx + dy * dy + dz * dz <= tolerance * tolerance &&
			forward.i * object->object.forward.i + forward.j * object->object.forward.j +
				forward.k * object->object.forward.k >= angle_tolerance &&
			up.i * object->object.up.i + up.j * object->object.up.j + up.k * object->object.up.k >= angle_tolerance)
		{
			continue;
		}
		if (network_objects_reconcile(state->object_index, &state->position, &forward, &up, &velocity,
			&angular_velocity, blend_distance))
		{
			distributed_count_correction();
		}
		SET_FLAG(object->object.flags, _object_at_rest_bit, TEST_FLAG(state->flags, _distributed_object_at_rest_bit));
	}
}

/* what this machine's own player's unit carries now noted (it is that
player's: local_player_index); compared with what it carried at the last,
when it threw a grenade and fired or reloaded a weapon, noted too */
static void distributed_client_note_own_inventory(
	struct distributed_own_inventory *own,
	long unit_index,
	boolean compare)
{
	struct unit_datum *unit = unit_get(unit_index);
	long now = game_time_get();
	short grenade_type;
	short weapon_slot;

	if (own->unit_index != unit_index)
	{
		own->unit_index = unit_index;
		compare = FALSE;
		for (grenade_type = 0; grenade_type < NUMBER_OF_UNIT_GRENADE_TYPES; grenade_type++)
			own->grenade_times[grenade_type] = NONE;
		for (weapon_slot = 0; weapon_slot < MAXIMUM_WEAPONS_PER_UNIT; weapon_slot++)
		{
			own->weapon_indices[weapon_slot] = NONE;
			own->weapon_times[weapon_slot] = NONE;
		}
	}
	for (grenade_type = 0; grenade_type < NUMBER_OF_UNIT_GRENADE_TYPES; grenade_type++)
	{
		if (compare && unit->unit.grenade_counts[grenade_type] < own->grenade_counts[grenade_type])
			own->grenade_times[grenade_type] = now;
		own->grenade_counts[grenade_type] = unit->unit.grenade_counts[grenade_type];
	}
	for (weapon_slot = 0; weapon_slot < MAXIMUM_WEAPONS_PER_UNIT; weapon_slot++)
	{
		long weapon_index = unit->unit.weapon_object_indices[weapon_slot];
		struct weapon_datum *weapon = weapon_index != NONE ? weapon_try_and_get(weapon_index) : NULL;
		short magazine;

		if (!weapon)
		{
			own->weapon_indices[weapon_slot] = NONE;
			continue;
		}
		/* (another weapon: none of it done here yet) */
		if (own->weapon_indices[weapon_slot] != weapon_index)
		{
			own->weapon_indices[weapon_slot] = weapon_index;
			own->weapon_times[weapon_slot] = NONE;
		}
		else if (compare)
		{
			boolean used = weapon->weapon.game_time_last_fired != own->fired_times[weapon_slot];

			for (magazine = 0; magazine < 2; magazine++)
			{
				used |= weapon->weapon.magazines[magazine].state == _magazine_reloading ||
					weapon->weapon.magazines[magazine].rounds_total != own->rounds[weapon_slot][magazine][0] ||
					weapon->weapon.magazines[magazine].rounds_loaded != own->rounds[weapon_slot][magazine][1];
			}
			if (used)
				own->weapon_times[weapon_slot] = now;
		}
		own->fired_times[weapon_slot] = weapon->weapon.game_time_last_fired;
		for (magazine = 0; magazine < 2; magazine++)
		{
			own->rounds[weapon_slot][magazine][0] = weapon->weapon.magazines[magazine].rounds_total;
			own->rounds[weapon_slot][magazine][1] = weapon->weapon.magazines[magazine].rounds_loaded;
		}
	}
}

/* whether the host has had a round trip (and the time its inventories take
to go) since this machine last did that with its own player's unit (at
time, NONE for never): what the host says of it then has what it did */
static boolean distributed_client_own_settled(
	long time)
{
	real round_trip = distributed_own_round_trip_ticks();

	if (time == NONE)
		return TRUE;
	/* (so written that one not a number is none) */
	if (!(round_trip > 0.0f))
		round_trip = (real)DEFAULT_OWN_ROUND_TRIP_TICKS;
	if (round_trip > (real)MAXIMUM_OWN_ROUND_TRIP_TICKS)
		round_trip = (real)MAXIMUM_OWN_ROUND_TRIP_TICKS;
	return game_time_get() - time > (long)ceil(round_trip) + INVENTORY_INTERVAL_TICKS + OWN_INVENTORY_MARGIN_TICKS;
}

/* the unit carries what the host's does */
static void distributed_client_apply_inventory(
	struct distributed_inventory const *inventory)
{
	struct unit_datum *unit;
	struct distributed_own_inventory *own = NULL;
	boolean local;
	short weapon_slot;

	if (!network_objects_client_has(inventory->unit_index) ||
		!object_try_and_get_and_verify_type(inventory->unit_index, _object_mask_unit))
	{
		return;
	}
	unit = unit_get(inventory->unit_index);
	if (TEST_FLAG(unit->object.damage_flags, _object_dead_bit))
		return;
	local = distributed_player_is_local(unit->unit.player_index);
	if (local)
	{
		short local_player_index = player_get(unit->unit.player_index)->local_player_index;

		if (local_player_index >= 0 && local_player_index < MAXIMUM_LOCAL_PLAYERS &&
			objects_client_own_inventories[local_player_index].unit_index == inventory->unit_index)
		{
			own = &objects_client_own_inventories[local_player_index];
		}
	}
	/* picked up, swapped or dropped on the host: the same weapons here,
	slot for slot (the host's own objects, moved in and out as
	unit_add_weapon_to_inventory and unit_drop_current_weapon do, without
	their rules: the host has applied them) */
	for (weapon_slot = 0; weapon_slot < MAXIMUM_WEAPONS_PER_UNIT; weapon_slot++)
	{
		long wanted_index = inventory->weapon_indices[weapon_slot];
		long weapon_index = unit->unit.weapon_object_indices[weapon_slot];

		if (weapon_index == wanted_index)
			continue;
		/* (not here yet: as it is, until it is) */
		if (wanted_index != NONE &&
			(!network_objects_client_has(wanted_index) || !object_try_and_get_and_verify_type(wanted_index, _object_mask_weapon)))
		{
			continue;
		}
		if (weapon_index != NONE)
			unit_network_drop_weapon(inventory->unit_index, weapon_slot);
		if (wanted_index != NONE)
		{
			distributed_client_release_item(wanted_index, FALSE);
			unit_network_add_weapon(inventory->unit_index, wanted_index, weapon_slot);
		}
	}
	/* the weapon in hand: a client's own player chooses its own, unless its
	choice is gone */
	if (inventory->current_weapon_index >= 0 && inventory->current_weapon_index < MAXIMUM_WEAPONS_PER_UNIT &&
		unit->unit.current_weapon_index != inventory->current_weapon_index &&
		(!local || unit->unit.current_weapon_index == NONE ||
			unit->unit.weapon_object_indices[unit->unit.current_weapon_index] == NONE))
	{
		unit->unit.desired_weapon_index = inventory->current_weapon_index;
	}
	/* the ammunition (as much as the weapon's magazines hold): a client's
	own player spends its own as it fires and reloads (the host's count
	trails it), so it is the host's only once the host has had a round trip
	to see what it did with the weapon; then whatever the host's is */
	for (weapon_slot = 0; weapon_slot < MAXIMUM_WEAPONS_PER_UNIT; weapon_slot++)
	{
		long weapon_index = unit->unit.weapon_object_indices[weapon_slot];
		struct weapon_datum *weapon = weapon_index != NONE ? weapon_try_and_get(weapon_index) : NULL;
		struct weapon_definition *definition;
		short magazine;

		if (!weapon || weapon_index != inventory->weapon_indices[weapon_slot])
			continue;
		if (own && own->weapon_indices[weapon_slot] == weapon_index &&
			!distributed_client_own_settled(own->weapon_times[weapon_slot]))
		{
			continue;
		}
		definition = weapon_definition_get(weapon->definition_index);
		for (magazine = 0; magazine < 2 && magazine < definition->weapon.magazines.count; magazine++)
		{
			struct weapon_magazine *state = &weapon->weapon.magazines[magazine];
			struct weapon_magazine_definition *magazine_definition = TAG_BLOCK_GET_ELEMENT(
				&definition->weapon.magazines, magazine, struct weapon_magazine_definition);
			short total_maximum = MAX(magazine_definition->rounds_total_maximum, magazine_definition->rounds_total_initial);
			short total = inventory->rounds_total[weapon_slot][magazine];
			short loaded = inventory->rounds_loaded[weapon_slot][magazine];

			state->rounds_total = total < 0 ? 0 : MIN(total, total_maximum);
			state->rounds_loaded = loaded < 0 ? 0 : MIN(loaded, magazine_definition->rounds_loaded_maximum);
		}
		weapon->weapon.age = (real)inventory->age[weapon_slot] / AGE_SCALE;
	}
	/* the grenades (no more than a unit carries): a client's own player
	throws its own, so the host's count is taken when it is lower, and when
	higher (a pickup) only once the host has had a round trip to see the last
	it threw */
	{
		short grenade_type;

		for (grenade_type = 0; grenade_type < NUMBER_OF_UNIT_GRENADE_TYPES; grenade_type++)
		{
			char count = inventory->grenade_counts[grenade_type] < 0 ? 0 :
				(char)MIN(inventory->grenade_counts[grenade_type], MAXIMUM_INVENTORY_GRENADES);

			if (!own || count < unit->unit.grenade_counts[grenade_type] ||
				distributed_client_own_settled(own->grenade_times[grenade_type]))
			{
				unit->unit.grenade_counts[grenade_type] = count;
			}
		}
	}
	/* (what the host changed here is not this machine's doing) */
	if (own)
		distributed_client_note_own_inventory(own, inventory->unit_index, FALSE);
}

void network_objects_handle_inventories(
	void const *entries,
	short count)
{
	struct distributed_inventory const *inventories = (struct distributed_inventory const *)entries;
	short index;

	for (index = 0; index < count; index++)
		distributed_client_apply_inventory(&inventories[index]);
}

void network_objects_set_seat(
	long unit_index,
	long vehicle_index,
	short seat_index)
{
	struct unit_datum *unit = (struct unit_datum *)object_try_and_get_and_verify_type(unit_index, _object_mask_unit);
	long occupant_index = NONE;

	if (!unit)
		return;
	/* (a seat the vehicle has) */
	if (vehicle_index != NONE)
	{
		struct unit_datum *vehicle;

		if (vehicle_index == unit_index || !distributed_object_index_valid(vehicle_index) ||
			!network_objects_client_has(vehicle_index))
		{
			return;
		}
		vehicle = (struct unit_datum *)object_try_and_get_and_verify_type(vehicle_index, _object_mask_vehicle);
		if (!vehicle || seat_index < 0 ||
			seat_index >= unit_definition_get(vehicle->definition_index)->unit.seats.count)
		{
			return;
		}
	}
	if (unit->object.parent_object_index != NONE && unit->unit.parent_seat_index != NONE)
		unit_exit_seat_end(unit_index);
	if (vehicle_index == NONE || unit->object.parent_object_index != NONE)
		return;
	/* (whoever this machine still has in the seat: out) */
	unit_can_enter_seat(unit_index, vehicle_index, seat_index, &occupant_index);
	if (occupant_index != NONE && occupant_index != unit_index)
		unit_exit_seat_end(occupant_index);
	unit_enter_seat(unit_index, vehicle_index, seat_index);
}

/* the vehicles its own players drive, to the host, with this tick (which
the host tells back, with where it has them), and where they are noted */
static void distributed_client_send_vehicles(
	void)
{
	struct distributed_object_state_message message;
	short count = 0;
	short local_player_index;

	for (local_player_index = 0; local_player_index < MAXIMUM_LOCAL_PLAYERS; local_player_index++)
	{
		long player_index = local_player_get_player_index(local_player_index);
		struct player_datum *player = player_index != NONE ? player_try_and_get(player_index) : NULL;
		long vehicle_index = player ? distributed_driven_vehicle(player) : NONE;
		struct distributed_own_vehicle *own =
			&objects_client_own_vehicles[local_player_index][game_time_get() & (OWN_VEHICLE_POSITION_TICKS - 1)];

		if (vehicle_index != NONE && (!network_objects_client_has(vehicle_index) ||
			object_get(vehicle_index)->object.parent_object_index != NONE))
		{
			vehicle_index = NONE;
		}
		own->time = game_time_get();
		own->vehicle_index = vehicle_index;
		if (vehicle_index == NONE)
			continue;
		own->position = object_get(vehicle_index)->object.position;
		distributed_state_from_object(vehicle_index, &message.states[count]);
		message.states[count++].time = (short)(word)game_time_get();
	}
	if (count)
	{
		distributed_send(&message, _distributed_message_vehicle_prediction, count,
			(word)(sizeof(message.header) + count * sizeof(struct distributed_object_state)), _distributed_to_host);
	}
}

/* what its own players' units carry, noted: when they threw grenades,
fired and reloaded */
static void distributed_client_note_own_inventories(
	void)
{
	short local_player_index;

	for (local_player_index = 0; local_player_index < MAXIMUM_LOCAL_PLAYERS; local_player_index++)
	{
		long player_index = local_player_get_player_index(local_player_index);
		struct player_datum *player = player_index != NONE ? player_try_and_get(player_index) : NULL;
		long unit_index = distributed_living_unit(player);

		if (unit_index == NONE)
			objects_client_own_inventories[local_player_index].unit_index = NONE;
		else
			distributed_client_note_own_inventory(&objects_client_own_inventories[local_player_index], unit_index, TRUE);
	}
}

/* the objects of the kinds the host has that it has not told of go (what
this machine's own simulation made): those it made since the last tick, or
all of them once the host has told it all (or when it cannot say which it
made) */
static void distributed_client_remove_own_objects(
	void)
{
	short index;

	if (objects_client_check_all)
	{
		struct object_iterator iterator;

		objects_client_check_all = FALSE;
		objects_client_new_object_count = 0;
		object_iterator_new(&iterator, NETWORKED_OBJECT_TYPES, 0);
		while (object_iterator_next(&iterator))
		{
			long absolute_index = DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.index);

			if (!distributed_object_networked(iterator.index) || objects_client_has[absolute_index] == iterator.index)
				continue;
			objects_statistics.own_objects_removed++;
			distributed_client_delete(iterator.index);
		}
		return;
	}
	for (index = 0; index < objects_client_new_object_count; index++)
	{
		long object_index = objects_client_new_objects[index];

		if (!distributed_object_networked(object_index) ||
			objects_client_has[DATUM_INDEX_TO_ABSOLUTE_INDEX(object_index)] == object_index)
		{
			continue;
		}
		objects_statistics.own_objects_removed++;
		distributed_client_delete(object_index);
	}
	objects_client_new_object_count = 0;
}

void network_objects_client_tick(
	void)
{
	/* (loaded: the host's objects, please; flagged when one of them failed
	since it last asked) */
	if (!objects_client_synchronized &&
		(objects_client_ready_time == NONE || game_time_get() - objects_client_ready_time >= objects_client_ready_interval))
	{
		struct distributed_message_header message;

		/* (asked, less often the longer the host takes: a slow link does
		not queue up the asking) */
		if (objects_client_ready_time != NONE)
			objects_client_ready_interval = MIN(2 * objects_client_ready_interval, CLIENT_READY_MAXIMUM_INTERVAL_TICKS);
		objects_client_ready_time = game_time_get();
		distributed_send(&message, _distributed_message_client_ready, objects_client_ask_again ? 1 : 0,
			(word)sizeof(message), _distributed_to_host_reliably);
		objects_client_ask_again = FALSE;
	}
	if (objects_client_synchronized)
		distributed_client_remove_own_objects();
	/* (too many to look at one by one later: all of them) */
	else if (objects_client_new_object_count >= MAXIMUM_CLIENT_NEW_OBJECTS)
		objects_client_check_all = TRUE;
	distributed_client_note_own_inventories();
	distributed_client_send_vehicles();
	/* (who it is, as its Discord told it: once its ready went, which makes
	it a machine the host takes messages of) */
	if (objects_client_ready_time != NONE)
		distributed_client_send_identity();
}

/* ---------- the game */

void network_objects_new_game(
	void)
{
	long absolute_index;
	short machine_index;
	short local_player_index;
	short index;

	csmemset(objects_host_inventories, 0, sizeof(objects_host_inventories));
	for (absolute_index = 0; absolute_index < MAXIMUM_TRACKED_OBJECTS; absolute_index++)
	{
		objects_host_told[absolute_index] = NONE;
		objects_host_state_moving[absolute_index] = FALSE;
		objects_host_inventories[absolute_index].carried_time = NONE;
		objects_host_inventories[absolute_index].ammunition_time = NONE;
		objects_client_has[absolute_index] = NONE;
		objects_client_dead[absolute_index].object_index = NONE;
	}
	objects_host_told_count = 0;
	objects_host_resting_cursor = 0;
	objects_host_viewers.count = 0;
	for (machine_index = 0; machine_index < HALO_PORT_MAXIMUM_NETWORK_MACHINES; machine_index++)
		objects_host_machines[machine_index].time = NONE;
	csmemset(objects_host_vehicle_predictions, 0, sizeof(objects_host_vehicle_predictions));
	for (index = 0; index < MAXIMUM_TRACKED_PLAYERS; index++)
	{
		objects_host_vehicle_predictions[index].accepted_vehicle_index = NONE;
		objects_host_vehicle_predictions[index].ground_vehicle_index = NONE;
		objects_host_vehicle_predictions[index].rest_vehicle_index = NONE;
	}
	objects_client_synchronized = FALSE;
	objects_client_ready_time = NONE;
	objects_client_resynchronizing = FALSE;
	csmemset(objects_client_resync_seen, 0, sizeof(objects_client_resync_seen));
	objects_client_ready_interval = CLIENT_READY_INTERVAL_TICKS;
	objects_client_failed_time = NONE;
	objects_client_retry_ticks = CLIENT_RETRY_TICKS;
	objects_client_ask_again = FALSE;
	objects_client_local_allocation = FALSE;
	objects_client_creating_index = NONE;
	objects_client_creating = FALSE;
	objects_client_deleting = FALSE;
	objects_client_new_object_count = 0;
	objects_client_check_all = TRUE;
	objects_client_local_free_index = LOCAL_OBJECTS_FIRST_INDEX;
	objects_client_object_count = NONE;
	for (local_player_index = 0; local_player_index < MAXIMUM_LOCAL_PLAYERS; local_player_index++)
	{
		objects_client_own_inventories[local_player_index].unit_index = NONE;
		for (index = 0; index < OWN_VEHICLE_POSITION_TICKS; index++)
		{
			objects_client_own_vehicles[local_player_index][index].time = NONE;
			objects_client_own_vehicles[local_player_index][index].vehicle_index = NONE;
		}
	}
}

/* A handover snapshot supplements the visible replica with authority-only
cooldowns, item ages, weapon reload/heat state and in-flight projectiles.
These records contain values and canonical object indices, never pointers,
cluster-list links, renderer caches, or local audio/effect identifiers. */
struct migration_object_state
{
	struct distributed_object_change identity;
	long shield_damage_decay_timer;
	long body_damage_decay_timer;
	short shield_stun_ticks;
	short type;
	long owner_object_index;
	long parent_object_index;
	short parent_node_index;
	short pad;
	struct _item_datum item;
	struct _weapon_datum weapon;
	struct _projectile_datum projectile;
};

long network_objects_migration_write(byte *buffer, long size)
{
	struct object_iterator iterator;
	struct object_datum *object;
	long count = 0;
	long offset = sizeof(count);
	if (size < offset)
		return 0;
	object_iterator_new(&iterator, NETWORKED_OBJECT_TYPES | _object_mask_projectile, 0);
	while ((object = object_iterator_next(&iterator)) != NULL)
	{
		struct migration_object_state state;
		if (size - offset < (long)sizeof(state))
			return 0;
		memset(&state, 0, sizeof(state));
		distributed_change_from_object(iterator.index, &state.identity);
		state.type = object->object.type;
		state.shield_damage_decay_timer = object->object.shield_damage_decay_timer;
		state.body_damage_decay_timer = object->object.body_damage_decay_timer;
		state.shield_stun_ticks = object->object.shield_stun_ticks;
		state.owner_object_index = object->object.owner_object_index;
		state.parent_object_index = object->object.parent_object_index;
		state.parent_node_index = object->object.parent_node_index;
		if (state.type == _object_type_projectile && state.parent_object_index != NONE)
		{
			object_get_origin(iterator.index, &state.identity.position);
			object_get_orientation(iterator.index, &state.identity.forward, &state.identity.up);
		}
		if (TEST_FLAG(_object_mask_item | _object_mask_projectile, state.type))
			state.item = ((struct item_datum *)object)->item;
		if (state.type == _object_type_weapon)
		{
			short trigger;
			state.weapon = ((struct weapon_datum *)object)->weapon;
			/* Effects are local allocations, even when their object is shared. */
			state.weapon.overheated_effect_index = NONE;
			for (trigger = 0; trigger < 2; trigger++)
				state.weapon.triggers[trigger].charging_effect_index = NONE;
		}
		if (state.type == _object_type_projectile)
		{
			state.projectile = ((struct projectile_datum *)object)->projectile;
			state.projectile.tracer_attachment_index = NONE;
		}
		memcpy(buffer + offset, &state, sizeof(state));
		offset += sizeof(state);
		count++;
	}
	memcpy(buffer, &count, sizeof(count));
	return offset;
}

boolean network_objects_migration_validate(byte const *buffer, long size)
{
	long count;
	long index;
	byte seen[MAXIMUM_TRACKED_OBJECTS];
	if (size < (long)sizeof(count))
		return FALSE;
	memcpy(&count, buffer, sizeof(count));
	if (count < 0 || count > MAXIMUM_TRACKED_OBJECTS ||
		size - (long)sizeof(count) != count * (long)sizeof(struct migration_object_state))
		return FALSE;
	memset(seen, 0, sizeof(seen));
	for (index = 0; index < count; index++)
	{
		struct migration_object_state state;
		long absolute;
		memcpy(&state, buffer + sizeof(count) + index * sizeof(state), sizeof(state));
		absolute = DATUM_INDEX_TO_ABSOLUTE_INDEX(state.identity.object_index);
		if (absolute < 0 || absolute >= MAXIMUM_TRACKED_OBJECTS || seen[absolute] ||
			state.type < 0 || state.type >= NUMBER_OF_OBJECT_TYPES ||
			!TEST_FLAG(NETWORKED_OBJECT_TYPES | _object_mask_projectile, state.type) ||
			state.identity.definition_index == NONE ||
			state.identity.owner_player_index != NO_PLAYER &&
			state.identity.owner_player_index >= MAXIMUM_TRACKED_PLAYERS)
			return FALSE;
		seen[absolute] = 1;
	}
	return TRUE;
}

static boolean network_objects_migration_weapon_local(struct object_datum const *object)
{
	struct unit_datum *owner = (struct unit_datum *)object_try_and_get_and_verify_type(
		object->object.parent_object_index, _object_mask_unit);
	return owner && distributed_player_is_local(owner->unit.player_index);
}

boolean network_objects_migration_restore(byte const *buffer, long size, long elapsed_ticks)
{
	long count;
	long index;
	if (!network_objects_migration_validate(buffer, size))
		return FALSE;
	memcpy(&count, buffer, sizeof(count));
	/* Locally predicted projectiles are outside the canonical world. Replace
them with the checkpoint's exact generations before assuming authority. */
	{
		struct object_iterator iterator;
		struct object_datum *object;
		object_iterator_new(&iterator, _object_mask_projectile, 0);
		while ((object = object_iterator_next(&iterator)) != NULL)
			distributed_client_delete(iterator.index);
	}
	for (index = 0; index < count; index++)
	{
		struct migration_object_state state;
		struct object_datum *object;
		long object_index;
		unsigned long item_flags = 0;
		memcpy(&state, buffer + sizeof(count) + index * sizeof(state), sizeof(state));
		object_index = state.identity.object_index;
		if (state.type == _object_type_projectile)
		{
			long absolute = DATUM_INDEX_TO_ABSOLUTE_INDEX(object_index);
			/* A newer canonical object took this slot after the checkpoint:
the generation wins. Never delete a surviving player's current object. */
			struct datum_header const *header = (struct datum_header const *)
				((byte const *)object_header_data->data + absolute * object_header_data->size);
			if (header->identifier)
				continue;
			distributed_client_create(&state.identity);
		}
		object = object_try_and_get(object_index);
		if (!object || object->definition_index != state.identity.definition_index || object->object.type != state.type)
			continue;
		object->object.shield_damage_decay_timer = state.shield_damage_decay_timer;
		object->object.body_damage_decay_timer = state.body_damage_decay_timer;
		object->object.shield_stun_ticks = state.shield_stun_ticks;
		if (TEST_FLAG(_object_mask_item | _object_mask_projectile, state.type))
		{
			item_flags = ((struct item_datum *)object)->item.flags;
			((struct item_datum *)object)->item = state.item;
		}
		if (state.type == _object_type_weapon)
		{
			struct _weapon_datum *weapon = &((struct weapon_datum *)object)->weapon;
			boolean keep_current = network_objects_migration_weapon_local(object);
			short magazine;
			/* Ammo, magazine/reload state and trigger flags are one coherent
tuple. Combining newer ammo with an old reload can discard a filled clip
on the next tick. Local prediction, or any newer inventory, wins together. */
			for (magazine = 0; magazine < 2; magazine++)
			{
				if (weapon->magazines[magazine].rounds_total != state.weapon.magazines[magazine].rounds_total ||
					weapon->magazines[magazine].rounds_loaded != state.weapon.magazines[magazine].rounds_loaded)
					keep_current = TRUE;
			}
			if (keep_current)
				((struct item_datum *)object)->item.flags = item_flags;
			else
				*weapon = state.weapon;
			if (weapon->tracked_object_index != NONE && !object_try_and_get(weapon->tracked_object_index))
				weapon->tracked_object_index = NONE;
		}
		if (state.type == _object_type_projectile)
		{
			object->object.owner_object_index = state.owner_object_index != NONE &&
				object_try_and_get(state.owner_object_index) ? state.owner_object_index : NONE;
			((struct projectile_datum *)object)->projectile = state.projectile;
			if (state.projectile.ignore_object_index != NONE && !object_try_and_get(state.projectile.ignore_object_index))
				((struct projectile_datum *)object)->projectile.ignore_object_index = NONE;
			if (state.projectile.target_object_index != NONE && !object_try_and_get(state.projectile.target_object_index))
				((struct projectile_datum *)object)->projectile.target_object_index = NONE;
			if (state.parent_object_index != NONE && object_try_and_get(state.parent_object_index))
				object_attach_to_node(state.parent_object_index, object_index, state.parent_node_index);
		}
	}
	/* Replay only unconfirmed projectile movement. Applying its damage again
would duplicate an explosion already reflected in the surviving world. */
	network_damage_migration_replay(TRUE);
	for (index = 0; index < count; index++)
	{
		struct migration_object_state state;
		long step;
		memcpy(&state, buffer + sizeof(count) + index * sizeof(state), sizeof(state));
		if (state.type != _object_type_projectile)
			continue;
		for (step = 0; step < elapsed_ticks; step++)
		{
			struct object_header_datum *header = object_header_try_and_get(state.identity.object_index);
			if (!header || !header->datum || TEST_FLAG(header->flags, _object_header_being_deleted_bit))
				break;
			projectile_update(state.identity.object_index);
		}
	}
	network_damage_migration_replay(FALSE);
	return TRUE;
}

void network_objects_migration_reset_transport(void)
{
	long absolute;
	/* A promoted host will reannounce the inherited objects. Existing clients
retain their canonical object generations and can accept those announcements. */
	for (absolute = 0; absolute < MAXIMUM_TRACKED_OBJECTS; absolute++)
		objects_host_told[absolute] = NONE;
	objects_host_resting_cursor = 0;
	memset(objects_host_inventories, 0, sizeof(objects_host_inventories));
	memset(objects_host_vehicle_predictions, 0, sizeof(objects_host_vehicle_predictions));
	if (network_game_distributed_client())
	{
		/* The new stream may have missed a spawn or delete. Ask for the
		existing host's complete object set, without loading a new match. */
		objects_client_synchronized = FALSE;
		objects_client_resynchronizing = TRUE;
		memset(objects_client_resync_seen, 0, sizeof(objects_client_resync_seen));
	}
	objects_client_ready_time = NONE;
	objects_client_creating_index = NONE;
	objects_client_creating = FALSE;
	objects_client_deleting = FALSE;
}
