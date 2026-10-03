/*
NETWORK_ACTORS.C

The host's actors' units on every machine (port/linux/NETCODE.md).

Only the host runs the AI (game.c skips ai_update on a client): its actors
decide where their units go, what they face and shoot at, and give it to
the units as a player's input is, through unit_control. A client has the
units (network_objects.c), but until now nothing drove them: they stood
where the host's corrections put them, never aiming, firing or animating.

Each tick the host notes the control every actor gives its unit (the hook
in unit_control), and sends each client those units' controls with their
state (where they are, what they ride, their shields and health), those
near the client's players every tick and the others less often, as it
sends the moving objects. A client gives each unit the latest control it
has for it, each tick, where the host would have run its actor: the unit
then moves, aims, fires, throws and melees here as it does on the host,
through the same code that drives a remote player's unit from the input
the host relays, and the host's state corrects what drifts. Animation
impulses (a dive, a leap, a vault) are told with a number that counts
them, so a client plays each once however many times it hears of it.

Deaths need nothing here: the host's damage kills a client's copy as it
kills a player's (network_damage.c).
*/

/* ---------- headers */

#include "cseries.h"
#include "game/game.h"
#include "networking/network_game_globals.h"
#include "objects/objects.h"
#include "objects/damage.h"
#include "units/units.h"
#include "units/unit_control_data.h"
#include "network_distributed.h"

#include <math.h>

/* ---------- constants */

enum
{
	/* units the host's actors drive at once */
	MAXIMUM_NETWORK_ACTORS = 128,
	MAXIMUM_ENTRIES_PER_MESSAGE = 64,
	/* a control is held this long after the host last sent it; past that
	the unit is left to stand (the host's actor let it go, or the host is
	silent) */
	CONTROL_HELD_TICKS = 2 * TICKS_PER_SECOND,
	/* an impulse goes in this many of the unit's entries after it happens,
	in case one is lost */
	IMPULSE_REPEAT_TICKS = 3,
	/* unit_start_animation_impulse's impulses (units.c, which keeps the count) */
	NUMBER_OF_UNIT_ANIMATION_IMPULSES = 14,
	NO_IMPULSE = 0xFF,
};

/* struct distributed_actor_state flags */
enum
{
	_distributed_actor_rides_bit = 0,
	_distributed_actor_shield_depleted_bit,
	_distributed_actor_shield_charging_bit,
	_distributed_actor_shield_over_charging_bit,
	_distributed_actor_camouflaged_bit,
	_distributed_actor_super_camouflaged_bit,
	/* the impulse aligns the unit's facing (impulse_alignment) */
	_distributed_actor_impulse_aligned_bit,
};

/* a unit's facing, aiming and looking vectors travel as yaw and pitch */
enum
{
	_angle_yaw,
	_angle_pitch,
	NUMBER_OF_ANGLES
};

/* ---------- structures */

/* the host's control of an actor's unit, and the unit's state as the tick
left it (what struct distributed_unit_state has of a player's) */
struct distributed_actor_state
{
	long unit_index;
	long vehicle_index;
	short seat_index;
	byte flags;
	/* counts the unit's animation impulses; NO_IMPULSE for none this entry */
	byte impulse_number;
	byte impulse;
	byte animation_state;
	byte aiming_speed;
	byte primary_trigger;
	word control_flags;
	signed char throttle[3];
	byte active_camouflage;
	short facing[NUMBER_OF_ANGLES];
	short aiming[NUMBER_OF_ANGLES];
	short looking[NUMBER_OF_ANGLES];
	short impulse_alignment;
	word body_vitality;
	word shield_vitality;
	real_point3d position;
	struct distributed_vector velocity;
	struct distributed_vector forward;
	struct distributed_vector up;
};

struct distributed_actor_state_message
{
	struct distributed_message_header header;
	struct distributed_actor_state states[MAXIMUM_ENTRIES_PER_MESSAGE];
};

/* (the host) what an actor gave its unit this tick */
struct host_actor
{
	long unit_index;
	long noted_time;
	struct unit_control_data control;
	/* the latest impulse, when it happened, and its count */
	short impulse;
	long impulse_time;
	byte impulse_number;
	boolean impulse_aligned;
	real_vector2d impulse_alignment;
};

/* (a client) the host's latest word on an actor's unit */
struct client_actor
{
	long unit_index;
	long received_time;
	struct unit_control_data control;
	/* the impulse to play, and the number of the last played */
	short impulse;
	byte impulse_number;
	byte played_impulse_number;
	boolean impulse_aligned;
	real_vector2d impulse_alignment;
	boolean driven;
};

/* ---------- globals */

static struct host_actor host_actors[MAXIMUM_NETWORK_ACTORS];
static short host_actor_count;

static struct client_actor client_actors[MAXIMUM_NETWORK_ACTORS];
static short client_actor_count;

/* ---------- private code */

static void angles_pack(
	real_vector3d const *vector,
	short *angles)
{
	angles[_angle_yaw] = distributed_angle_pack((real)atan2(vector->j, vector->i));
	angles[_angle_pitch] = distributed_angle_pack((real)asin(PIN(vector->k, -1.0f, 1.0f)));
}

static void angles_unpack(
	short const *angles,
	real_vector3d *vector)
{
	real yaw = distributed_angle_unpack(angles[_angle_yaw], FALSE);
	real pitch = distributed_angle_unpack(angles[_angle_pitch], TRUE);

	vector->i = (real)(cos(pitch) * cos(yaw));
	vector->j = (real)(cos(pitch) * sin(yaw));
	vector->k = (real)sin(pitch);
}

/* a unit's entry in a table, or a free one added for it, or NULL */
static struct host_actor *host_actor_for(
	long unit_index)
{
	short index;

	for (index = 0; index < host_actor_count; index++)
	{
		if (host_actors[index].unit_index == unit_index)
			return &host_actors[index];
	}
	if (host_actor_count >= MAXIMUM_NETWORK_ACTORS)
		return NULL;
	csmemset(&host_actors[host_actor_count], 0, sizeof(host_actors[0]));
	host_actors[host_actor_count].unit_index = unit_index;
	host_actors[host_actor_count].impulse = NONE;
	host_actors[host_actor_count].impulse_time = NONE;
	return &host_actors[host_actor_count++];
}

static struct client_actor *client_actor_for(
	long unit_index)
{
	short index;

	for (index = 0; index < client_actor_count; index++)
	{
		if (client_actors[index].unit_index == unit_index)
			return &client_actors[index];
	}
	if (client_actor_count >= MAXIMUM_NETWORK_ACTORS)
		return NULL;
	csmemset(&client_actors[client_actor_count], 0, sizeof(client_actors[0]));
	client_actors[client_actor_count].unit_index = unit_index;
	client_actors[client_actor_count].impulse = NONE;
	return &client_actors[client_actor_count++];
}

/* whether the unit is still one the host's actors drive: there, alive, no
player's */
static boolean actor_unit_valid(
	long unit_index)
{
	struct unit_datum *unit;

	if (!object_try_and_get_and_verify_type(unit_index, _object_mask_unit))
		return FALSE;
	unit = unit_get(unit_index);
	return !TEST_FLAG(unit->object.damage_flags, _object_dead_bit) && unit->unit.player_index == NONE;
}

/* (the host) the entry sent of an actor's unit */
static void actor_state_from_unit(
	struct host_actor const *actor,
	long now,
	struct distributed_actor_state *state)
{
	struct unit_datum *unit = unit_get(actor->unit_index);
	struct damage_network_state damage;
	real camouflage = PIN(unit->unit.active_camouflage, 0.0f, 1.0f);

	csmemset(state, 0, sizeof(*state));
	state->unit_index = actor->unit_index;
	state->vehicle_index = NONE;
	state->seat_index = NONE;
	if (unit->object.parent_object_index != NONE && unit->unit.parent_seat_index != NONE)
	{
		SET_FLAG(state->flags, _distributed_actor_rides_bit, TRUE);
		state->vehicle_index = unit->object.parent_object_index;
		state->seat_index = unit->unit.parent_seat_index;
	}
	state->animation_state = (byte)actor->control.animation_state;
	state->aiming_speed = (byte)actor->control.aiming_speed;
	state->primary_trigger = (byte)(long)floor(PIN(actor->control.primary_trigger, 0.0f, 1.0f) * 255.0f + 0.5f);
	state->control_flags = actor->control.control_flags;
	state->throttle[0] = (signed char)(long)floor(PIN(actor->control.throttle.i, -1.0f, 1.0f) * 127.0f + 0.5f);
	state->throttle[1] = (signed char)(long)floor(PIN(actor->control.throttle.j, -1.0f, 1.0f) * 127.0f + 0.5f);
	state->throttle[2] = (signed char)(long)floor(PIN(actor->control.throttle.k, -1.0f, 1.0f) * 127.0f + 0.5f);
	angles_pack(&actor->control.facing_vector, state->facing);
	angles_pack(&actor->control.aiming_vector, state->aiming);
	angles_pack(&actor->control.looking_vector, state->looking);
	state->impulse = NO_IMPULSE;
	if (actor->impulse != NONE && now - actor->impulse_time < IMPULSE_REPEAT_TICKS)
	{
		state->impulse = (byte)actor->impulse;
		state->impulse_number = actor->impulse_number;
		SET_FLAG(state->flags, _distributed_actor_impulse_aligned_bit, actor->impulse_aligned);
		if (actor->impulse_aligned)
		{
			state->impulse_alignment = distributed_angle_pack(
				(real)atan2(actor->impulse_alignment.j, actor->impulse_alignment.i));
		}
	}
	damage_get_network_state(actor->unit_index, &damage);
	SET_FLAG(state->flags, _distributed_actor_shield_depleted_bit, damage.shield_depleted);
	SET_FLAG(state->flags, _distributed_actor_shield_charging_bit, damage.shield_charging);
	SET_FLAG(state->flags, _distributed_actor_shield_over_charging_bit, damage.shield_over_charging);
	state->body_vitality = distributed_vitality_pack(damage.body_vitality);
	state->shield_vitality = distributed_vitality_pack(damage.shield_vitality);
	SET_FLAG(state->flags, _distributed_actor_camouflaged_bit,
		TEST_FLAG(unit->unit.flags, _unit_active_camouflaged_bit));
	SET_FLAG(state->flags, _distributed_actor_super_camouflaged_bit,
		TEST_FLAG(unit->unit.flags, _unit_super_camouflaged_bit));
	state->active_camouflage = (byte)(long)floor(camouflage * 255.0f + 0.5f);
	state->position = unit->object.position;
	distributed_vector_pack(&unit->object.translational_velocity, DISTRIBUTED_VELOCITY_SCALE, &state->velocity);
	distributed_vector_pack(&unit->object.forward, DISTRIBUTED_UNIT_SCALE, &state->forward);
	distributed_vector_pack(&unit->object.up, DISTRIBUTED_UNIT_SCALE, &state->up);
}

/* (a client) the entry's control and state, given to its unit; FALSE for
an entry that says something no unit can be given */
static boolean actor_state_apply(
	struct distributed_actor_state const *state,
	long now)
{
	struct client_actor *actor;
	struct unit_datum *unit;
	struct damage_network_state damage;
	real_vector3d velocity;
	real_vector3d forward;
	real_vector3d up;

	if (!distributed_object_index_valid(state->unit_index) || !network_objects_client_has(state->unit_index) ||
		!actor_unit_valid(state->unit_index) ||
		state->animation_state >= NUMBER_OF_UNIT_ANIMATION_STATES ||
		state->aiming_speed >= NUMBER_OF_UNIT_AIMING_SPEEDS ||
		!VALID_FLAGS(state->control_flags, NUMBER_OF_UNIT_CONTROL_FLAGS) ||
		(state->impulse != NO_IMPULSE && state->impulse >= NUMBER_OF_UNIT_ANIMATION_IMPULSES) ||
		!distributed_point_valid(&state->position, UNIT_WORLD_BOUND))
	{
		return FALSE;
	}
	distributed_vector_unpack(&state->velocity, DISTRIBUTED_VELOCITY_SCALE, &velocity);
	distributed_vector_unpack(&state->forward, DISTRIBUTED_UNIT_SCALE, &forward);
	distributed_vector_unpack(&state->up, DISTRIBUTED_UNIT_SCALE, &up);
	if (!distributed_axes_make_valid(&forward, &up))
		return FALSE;
	actor = client_actor_for(state->unit_index);
	if (!actor)
		return TRUE;

	/* the control, for the ticks until the next */
	actor->received_time = now;
	actor->control.animation_state = (char)state->animation_state;
	actor->control.aiming_speed = (char)state->aiming_speed;
	actor->control.control_flags = state->control_flags;
	actor->control.weapon_index = NONE;
	actor->control.grenade_index = NONE;
	actor->control.zoom_level = NONE;
	actor->control.throttle.i = (real)state->throttle[0] / 127.0f;
	actor->control.throttle.j = (real)state->throttle[1] / 127.0f;
	actor->control.throttle.k = (real)state->throttle[2] / 127.0f;
	actor->control.primary_trigger = (real)state->primary_trigger / 255.0f;
	angles_unpack(state->facing, &actor->control.facing_vector);
	angles_unpack(state->aiming, &actor->control.aiming_vector);
	angles_unpack(state->looking, &actor->control.looking_vector);
	if (state->impulse != NO_IMPULSE && state->impulse_number != actor->played_impulse_number)
	{
		real yaw = distributed_angle_unpack(state->impulse_alignment, FALSE);

		actor->impulse = state->impulse;
		actor->impulse_number = state->impulse_number;
		actor->impulse_aligned = TEST_FLAG(state->flags, _distributed_actor_impulse_aligned_bit);
		actor->impulse_alignment.i = (real)cos(yaw);
		actor->impulse_alignment.j = (real)sin(yaw);
	}

	/* the state: the seat, the shields and health, the camouflage, and
	where it is (a rider is where its vehicle is) */
	unit = unit_get(state->unit_index);
	if (TEST_FLAG(state->flags, _distributed_actor_rides_bit))
	{
		if (unit->object.parent_object_index != state->vehicle_index ||
			unit->unit.parent_seat_index != state->seat_index)
		{
			network_objects_set_seat(state->unit_index, state->vehicle_index, state->seat_index);
		}
	}
	else if (unit->object.parent_object_index != NONE)
	{
		network_objects_set_seat(state->unit_index, NONE, NONE);
	}
	damage_get_network_state(state->unit_index, &damage);
	damage.shield_depleted = TEST_FLAG(state->flags, _distributed_actor_shield_depleted_bit);
	damage.shield_charging = TEST_FLAG(state->flags, _distributed_actor_shield_charging_bit);
	damage.shield_over_charging = TEST_FLAG(state->flags, _distributed_actor_shield_over_charging_bit);
	damage.body_vitality = distributed_vitality_unpack(state->body_vitality);
	damage.shield_vitality = distributed_vitality_unpack(state->shield_vitality);
	damage_set_network_state(state->unit_index, &damage);
	SET_FLAG(unit->unit.flags, _unit_active_camouflaged_bit,
		TEST_FLAG(state->flags, _distributed_actor_camouflaged_bit));
	SET_FLAG(unit->unit.flags, _unit_super_camouflaged_bit,
		TEST_FLAG(state->flags, _distributed_actor_super_camouflaged_bit));
	unit->unit.active_camouflage = (real)state->active_camouflage / 255.0f;
	if (unit->object.parent_object_index == NONE)
	{
		real dx = state->position.x - unit->object.position.x;
		real dy = state->position.y - unit->object.position.y;
		real dz = state->position.z - unit->object.position.z;

		if (dx * dx + dy * dy + dz * dz > REMOTE_CORRECTION_TOLERANCE * REMOTE_CORRECTION_TOLERANCE &&
			network_objects_reconcile(state->unit_index, &state->position, &forward, &up, &velocity, NULL,
				REMOTE_BLEND_DISTANCE))
		{
			distributed_count_correction();
		}
	}
	return TRUE;
}

/* ---------- public code */

void network_actors_new_game(
	void)
{
	host_actor_count = 0;
	client_actor_count = 0;
}

/* (the host) an actor's unit was given this control (unit_control) */
void network_actors_note_control(
	long unit_index,
	struct unit_control_data const *control_data)
{
	struct host_actor *actor;

	if (game_connection() != _game_connection_network_server)
		return;
	actor = host_actor_for(unit_index);
	if (!actor)
		return;
	actor->noted_time = game_time_get();
	actor->control = *control_data;
}

/* (the host) an actor's unit began this animation impulse
(unit_start_animation_impulse) */
void network_actors_note_impulse(
	long unit_index,
	short animation_impulse,
	real_vector2d const *alignment_vector)
{
	struct host_actor *actor;

	if (game_connection() != _game_connection_network_server)
		return;
	actor = host_actor_for(unit_index);
	if (!actor)
		return;
	actor->impulse = animation_impulse;
	actor->impulse_time = game_time_get();
	actor->impulse_number++;
	actor->impulse_aligned = alignment_vector != NULL;
	if (alignment_vector)
		actor->impulse_alignment = *alignment_vector;
}

/* (the host, after each tick) to each client the units its actors drove
this tick: those near the client's players every tick, the others less
often, and any with a fresh impulse at once. A unit no actor drove this
tick is forgotten. */
void network_actors_host_tick(
	void)
{
	static struct distributed_actor_state states[MAXIMUM_NETWORK_ACTORS];
	struct distributed_actor_state_message message;
	long machine_indices[HALO_PORT_MAXIMUM_NETWORK_MACHINES];
	short machine_count = distributed_client_machines(machine_indices, HALO_PORT_MAXIMUM_NETWORK_MACHINES);
	long now = game_time_get();
	short state_count = 0;
	short index;
	short machine_number;

	for (index = 0; index < host_actor_count; index++)
	{
		struct host_actor *actor = &host_actors[index];

		if (actor->noted_time != now || !actor_unit_valid(actor->unit_index))
		{
			host_actors[index--] = host_actors[--host_actor_count];
			continue;
		}
		actor_state_from_unit(actor, now, &states[state_count++]);
	}
	for (machine_number = 0; machine_number < machine_count; machine_number++)
	{
		long machine_index = machine_indices[machine_number];
		short count = 0;

		for (index = 0; index < state_count; index++)
		{
			struct distributed_actor_state const *state = &states[index];
			short period = network_objects_send_period(machine_index, &state->position);

			if (state->impulse == NO_IMPULSE && (now + DATUM_INDEX_TO_ABSOLUTE_INDEX(state->unit_index)) % period != 0)
				continue;
			message.states[count++] = *state;
			if (count == MAXIMUM_ENTRIES_PER_MESSAGE || count == DATAGRAM_ENTRIES(struct distributed_actor_state))
			{
				distributed_send_to_machine(machine_index, &message, _distributed_message_actor_states, count,
					(word)(sizeof(message.header) + count * sizeof(struct distributed_actor_state)));
				count = 0;
			}
		}
		if (count)
		{
			distributed_send_to_machine(machine_index, &message, _distributed_message_actor_states, count,
				(word)(sizeof(message.header) + count * sizeof(struct distributed_actor_state)));
		}
	}
}

word network_actors_entry_size(
	void)
{
	return sizeof(struct distributed_actor_state);
}

/* (a client) the host's word on its actors' units */
void network_actors_handle_states(
	void const *entries,
	short count)
{
	struct distributed_actor_state const *states = entries;
	long now = game_time_get();
	short index;

	for (index = 0; index < count; index++)
		actor_state_apply(&states[index], now);
}

/* (a client, in its tick where the host runs its actors) each actor's unit
given the control the host last sent for it */
void network_actors_drive(
	void)
{
	long now = game_time_get();
	short index;

	for (index = 0; index < client_actor_count; index++)
	{
		struct client_actor *actor = &client_actors[index];

		if (!actor_unit_valid(actor->unit_index) || now - actor->received_time > CONTROL_HELD_TICKS)
		{
			if (actor->driven && object_try_and_get_and_verify_type(actor->unit_index, _object_mask_unit))
				unit_set_actively_controlled(actor->unit_index, FALSE);
			client_actors[index--] = client_actors[--client_actor_count];
			continue;
		}
		if (!actor->driven)
		{
			unit_set_actively_controlled(actor->unit_index, TRUE);
			actor->driven = TRUE;
		}
		unit_control(actor->unit_index, &actor->control);
		if (actor->impulse != NONE && actor->impulse_number != actor->played_impulse_number)
		{
			unit_start_animation_impulse(actor->unit_index, actor->impulse,
				actor->impulse_aligned ? &actor->impulse_alignment : NULL);
			actor->played_impulse_number = actor->impulse_number;
			actor->impulse = NONE;
		}
	}
}
