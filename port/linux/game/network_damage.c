/*
NETWORK_DAMAGE.C

The distributed netcode's damage (port/linux/NETCODE.md, stage 5, and what
being hit looks and feels like on the clients).

- The host deals all damage. Every tick it sends its clients the damage it
  dealt to units: whom, with what, from where, how much, and to what
  effect. A client replays what that damage does beside the harm (the
  host's shields and health come with the units' states): a player's
  screen flash and shake, the unit's flinch, pain sound, knockback and
  stun, the scope it was knocked out of, and whom the HUD shows it was hit
  by. A killing blow it replays whole, so that the body falls as the shot
  had it and the kill is announced with the host's killer (an actor's, no
  player's, counted by no one there).
- A client deals no damage itself. What its own players' shots, grenades,
  melee and vehicles hit it reports to the host, which deals it once it has
  checked it: the report is of that machine's player, the damage one that
  player's weapons (now or lately), grenades or the vehicle they drove
  deal, of the shape the game gives it (at a point, a melee blow from the
  striker, a collision from the vehicle), the target about where the host
  had it when the shooter saw it (the host keeps a second of where
  players' units and vehicles were, and looks back as far as the report
  was made: the host's tick the client had last heard of then; one older
  than that second is refused, as a burst of reports held back is), a
  melee blow from where the host had the player and the target within
  their reach, a collision from where the host had their vehicle, the
  impact at the target, and no more reports than the weapon that deals
  them fires (an explosion's hits are one, each object hit once). What the
  shooter saw hit, hits. The host's own copies of a client's projectiles
  deal nothing (the client's report does), unless that client has left
  the game.
- The damage to a unit goes to the machines of the players it concerns
  (the unit's, its riders', the damage's owner's) and those sent the
  unit's player this tick, which see it; a killing blow to all.
*/

#include "cseries.h"
#include "game/game.h"
#include "game/game_globals.h"
#include "game/players.h"
#include "networking/network_game_globals.h"
#include "objects/objects.h"
#include "objects/damage.h"
#include "objects/damage_effect_definitions.h"
#include "effects/effect_definitions.h"
#include "physics/collision_model_definitions.h"
#include "items/weapon_definitions.h"
#include "items/projectile_definitions.h"
#include "scenario/scenario.h"
#include "units/units.h"
#include "network_distributed.h"

#include <math.h>

/* damage.c's */
void damage_replay_player_effect(long player_index, struct damage_data *damage, real total_damage);
void damage_replay_aftermath(long object_index, struct damage_data *damage, unsigned long being_damaged_flags,
	real shield_damage, real body_damage, real body_damage_multiplier, short body_part);
void damage_replay_kill(long object_index, struct damage_data *damage, short node_index, short region_index,
	short material_index);
/* network_distributed.c's */
void distributed_set_death(short dead_player_index, byte killing_player_index, boolean friendly_fire,
	boolean killed_by_vehicle);
boolean distributed_get_death(short dead_player_index, byte *killing_player_index, boolean *friendly_fire,
	boolean *killed_by_vehicle);
/* cache_files.c's */
boolean tag_index_is_group(long tag_index, long group_tag);
/* game_engine.c's */
long game_engine_network_player_score(long player_index);
/* physics.c's (world units a tick, each tick) */
extern real global_gravity;

enum
{
	MAXIMUM_DAMAGE_EVENTS_PER_TICK = 256,
	MAXIMUM_HIT_REPORTS_PER_TICK = 256,
	MAXIMUM_ENTRIES_PER_MESSAGE = 64,
	/* the weapons a player has carried lately, whose damage the host takes
	from them (a grenade lands, a rocket arrives, after its thrower has
	died or dropped the launcher) */
	MAXIMUM_RECENT_WEAPONS = 8,
	RECENT_WEAPON_TICKS = 10 * TICKS_PER_SECOND,
	/* a player's hits: a bucket of this many seconds of fire, filling at a
	second a second, a hit taking what one of its weapon's projectiles
	takes to fire (at twice the weapon's fastest rate: a shotgun's
	pellets, a spray); an explosion's hits are one, however many it hits */
	HIT_REPORT_BURST_SECONDS = 3,
	HIT_REPORT_RATE_MARGIN = 2,
	/* the hits a second of what no trigger's rate says: melee, a
	grenade, a vehicle, a weapon's own detonation */
	MELEE_HITS_PER_SECOND = 4,
	GRENADE_HITS_PER_SECOND = 4,
	VEHICLE_HITS_PER_SECOND = 15,
	EFFECT_HITS_PER_SECOND = 4,
	/* the explosions' hits of a tick told apart, a player's (an explosion
	and an object each) */
	MAXIMUM_EXPLOSIONS_PER_TICK = 64,
	/* what weapons and grenades deal, remembered (a power of two) */
	DAMAGE_RATE_CACHE_SIZE = 1024,
	/* the damage events kept for killing blows */
	RESERVED_KILL_EVENTS = 64,
	/* tags within tags followed looking for a damage effect */
	MAXIMUM_TAG_DEPTH = 4,
	/* the ticks of where players' units and vehicles were, a power of two
	(about a second) */
	TARGET_HISTORY_TICKS = 32,
	/* ... looked back over beyond the host's tick a report was made at (the
	frame drawn a tick behind, the report's own tick) */
	TARGET_HISTORY_SLACK_TICKS = 3,
	/* ... and how old a report may be (a slow link's round trip, a report
	sent again): older, it is refused, as a burst after the network was lost
	is; one older than the history is looked back over as far as it goes */
	REPORT_MAXIMUM_AGE_TICKS = 3 * TICKS_PER_SECOND,
	/* the players whose machines a unit's damage goes to, told (the unit's,
	its riders', its owner's): with more, it goes to every machine */
	MAXIMUM_EVENT_PLAYERS = 8,
	/* where each player's unit was, every so many ticks, and how many of
	those are kept (a power of two: about thirteen seconds; what they fire
	outlives the history: a rocket's flight, a grenade its thrower) */
	PLAYER_TRAIL_INTERVAL_TICKS = 3,
	/* ... looked over from no earlier than the report's machine's round
	trip and this before now (a report made longer ago, sent again after a
	loss, has its shooter where they were then; one that says it was made
	long ago does not look further back than that) */
	PLAYER_TRAIL_REPORT_SLACK_TICKS = TICKS_PER_SECOND,
	PLAYER_TRAIL_POINTS = 128,
	PLAYER_TRAIL_TICKS = PLAYER_TRAIL_INTERVAL_TICKS * PLAYER_TRAIL_POINTS,
};

enum
{
	_damage_event_player_effect,
	_damage_event_aftermath,
	_damage_event_kill,
};

/* what a player's damage is, by how the game deals it (the shape of their
report of it) */
enum
{
	/* a projectile's impact, or its detonation on what it sticks to: at the
	point it hits (the origin the epicenter) */
	_damage_source_impact_bit = 0,
	/* an effect's (an explosion's): to what is within its reach, from the
	point it goes off (the origin the epicenter), area damage */
	_damage_source_area_bit,
	/* a weapon's melee blow: from the striker's head (the origin) and body
	(the epicenter), area damage (unit_cause_player_melee_damage) */
	_damage_source_melee_bit,
	/* the vehicle they drive running into a unit: at the unit (the origin),
	from the vehicle (the epicenter), area damage (physics.c) */
	_damage_source_collision_bit,
};

/* struct distributed_damage_event kill flags */
enum
{
	_damage_event_friendly_fire_bit = 0,
	_damage_event_killed_by_vehicle_bit,
	/* a telefrag (players_update_after_game): its message to the player */
	_damage_event_telefragged_bit,
};

/* what damage.c and projectiles.c have their tags' flags and choices as */
enum
{
	/* struct damage_effect_definition flags */
	_damage_effect_dont_scale_damage_by_distance_bit = 0,
};
enum
{
	/* struct damage_definition flags */
	_damage_detonates_explosives_bit = 5,
	_damage_only_hurts_shields_bit,
	_damage_skips_shields_bit = 9,
};
enum
{
	/* struct damage_resistance flags */
	_damage_resistance_takes_shield_damage_for_children_bit = 0,
	_damage_resistance_takes_body_damage_for_children_bit,
	_damage_resistance_parent_never_takes_body_damage_for_us_bit = 4,
	_damage_resistance_only_hurt_by_explosives_bit,
};
enum
{
	/* struct _projectile_definition detonation_timer_starts */
	_projectile_detonation_timer_starts_immediately = 0,
};

/* world units: how far the host may have the target from where the
shooter saw it (with how far it moves in half a second), and the impact
from the target */
#define REPORT_TARGET_TOLERANCE 3.0f
#define REPORT_TARGET_LEAD_TICKS 15.0f
#define REPORT_IMPACT_TOLERANCE 2.0f
/* ... and from where the host had a player's unit or vehicle then (with how
far it moves in a few ticks: a client's copy runs a little ahead of the
host's word on it), and further for a vehicle's (its size, and as far as a
client's own vehicle is from the host's before it is put right,
network_objects.c) */
#define REPORT_HISTORY_TOLERANCE 2.0f
#define REPORT_HISTORY_LEAD_TICKS 3.0f
#define REPORT_VEHICLE_TOLERANCE 4.0f
/* ... a unit a vehicle runs into from the vehicle: their sizes, and the push
it gives the unit (twice its speed, physics.c) and a tick of it */
#define REPORT_COLLISION_LEAD_TICKS 3.0f
/* ... and what a player fires from where the host had them (a client's
player is where it says within a tolerance of the host's, a weapon's muzzle
is off the unit; a vehicle's the vehicle's size more) */
#define REPORT_RANGE_TOLERANCE 6.0f
/* how far apart the origin and the epicenter of what hits at a point are
(one point, as the client had it) */
#define REPORT_POINT_TOLERANCE 0.01f
/* how far from the origin anything in a report is (world units), and the
largest damage scale a client's hit has: a melee blow's, airborne
(unit_update_melee), anything else's all of it */
#define REPORT_WORLD_BOUND 32768.0f
#define REPORT_MAXIMUM_MELEE_SCALE 1.5f

/* the damage flags a client's hit has: what its own object_cause_damage is
given (neither the host's instant kills nor its passengers' passthrough) */
#define REPORT_DAMAGE_FLAGS (FLAG(_damage_area_of_effect_bit) | FLAG(_damage_create_localized_effect_bit) | \
	FLAG(_damage_from_weapon_bit) | FLAG(_damage_damaged_one_object_bit))

/* the host's struct damage_data, as the other machines have it */
struct distributed_damage
{
	long definition_index;
	unsigned long flags;
	byte owner_player_index;
	byte pad;
	short owner_team_index;
	long owner_object_index;
	real_point3d origin;
	real_point3d epicenter;
	real_vector3d direction;
	real scale;
	real multiplier;
	real material_effect_scale;
	short material_type;
	short pad1;
};

typedef char distributed_damage_size_assert[sizeof(struct distributed_damage) == 0x44 ? 1 : -1];
typedef char distributed_damage_scale_offset_assert[offsetof(struct distributed_damage, scale) == 0x34 ? 1 : -1];

struct distributed_damage_event
{
	byte kind;
	/* the player effect's player; the killing blow's killer */
	byte player_index;
	byte kill_flags;
	byte pad;
	long object_index;
	struct distributed_damage damage;
	unsigned long being_damaged_flags;
	real shield_damage;
	real body_damage;
	real body_damage_multiplier;
	real total_damage;
	short body_part;
	short node_index;
	short region_index;
	short material_index;
	/* the killing blow's killer's score after it (as the host's game type
	has it: its message shows it before the game type's state comes) */
	long killer_score;
};

typedef char distributed_damage_event_size_assert[sizeof(struct distributed_damage_event) == 0x6C ? 1 : -1];
typedef char distributed_damage_event_damage_offset_assert[
	offsetof(struct distributed_damage_event, damage) == 8 ? 1 : -1];

struct distributed_hit_report
{
	long object_index;
	struct distributed_damage damage;
	/* where the shooter had the target */
	real_point3d target_position;
	real_vector3d object_normal;
	short node_index;
	short region_index;
	short material_index;
	boolean has_normal;
	byte pad;
	/* the host's latest tick the client had heard of when it made the report
	(the host looks back as far as that) */
	long host_time;
};

typedef char distributed_hit_report_size_assert[sizeof(struct distributed_hit_report) == 0x6C ? 1 : -1];
typedef char distributed_hit_report_host_time_offset_assert[
	offsetof(struct distributed_hit_report, host_time) == 0x68 ? 1 : -1];

struct distributed_damage_event_message
{
	struct distributed_message_header header;
	struct distributed_damage_event events[MAXIMUM_ENTRIES_PER_MESSAGE];
};

struct distributed_hit_report_message
{
	struct distributed_message_header header;
	struct distributed_hit_report reports[MAXIMUM_ENTRIES_PER_MESSAGE];
};

/* how far from its firer what a source deals goes off (world units), in
how many ticks from when it is fired, and for how many of them what it
fires is carried at its firer's own speed (a grenade thrown on the run); a
distance below 0 where the tags do not bound it */
struct damage_reach
{
	real distance;
	real ticks;
	real carried_ticks;
};

/* the vehicle damage in the globals' falling damage block, as vehicles.c
has it */
struct distributed_falling_damage
{
	byte unused0[0x2c];
	struct tag_reference maximum_distance_damage;
	struct tag_reference vehicle_hit_environment_damage_effect;
	struct tag_reference vehicle_killed_unit_damage_effect;
	struct tag_reference vehicle_collision_damage;
	struct tag_reference flaming_death_damage;
};

/* ---------- globals */

/* the host: the damage dealt this tick */
static struct distributed_damage_event damage_events[MAXIMUM_DAMAGE_EVENTS_PER_TICK];
static short damage_event_count;
/* ... dealing a client's report */
static boolean damage_dealing_report;
/* ... each player's weapons of late, and hits left */
static struct
{
	long definition_indices[MAXIMUM_RECENT_WEAPONS];
	long times[MAXIMUM_RECENT_WEAPONS];
	long grenade_times[NUMBER_OF_UNIT_GRENADE_TYPES];
	long driven_time;
	/* (the blow of the player's unit with no weapon: units.c's
	unit_unarmed_melee_damage) */
	long unarmed_melee_damage_index;
	long unarmed_time;
	real hit_seconds;
	long hit_seconds_time;
} damage_players[MAXIMUM_TRACKED_PLAYERS];
/* ... the explosions whose hits came this tick, each paid for once, and
the objects each hit (each once), each player's */
static struct
{
	long time;
	short count;
	struct
	{
		long definition_index;
		real_point3d epicenter;
		long object_index;
	} explosions[MAXIMUM_EXPLOSIONS_PER_TICK];
} damage_explosions[MAXIMUM_TRACKED_PLAYERS];
/* ... where each player's unit and vehicle were at each of the last ticks
(and whether they drove it) */
static struct damage_history_tick
{
	long time;
	struct
	{
		long object_index;
		real_point3d position;
		real speed;
		boolean driving;
	} objects[MAXIMUM_TRACKED_PLAYERS][2];
} damage_history[TARGET_HISTORY_TICKS];
/* ... and where each player's living unit was every few ticks, longer (NONE
for no time: none), how fast it or its vehicle went, and the size of the
vehicle it rode */
static struct damage_trail_point
{
	long time;
	real_point3d position;
	real speed;
	real radius;
} damage_trails[MAXIMUM_TRACKED_PLAYERS][PLAYER_TRAIL_POINTS];
/* ... what each weapon or grenade deals, as the tags have it (their walk
is long): the hits a second of a damage effect from it, how it deals it
(the _damage_source flags), and how far from its firer */
static struct damage_rate_cache_entry
{
	long source_index;
	long damage_index;
	real rate;
	byte kinds;
	struct damage_reach reach;
} damage_rate_cache[DAMAGE_RATE_CACHE_SIZE];
/* ... the players each damage event of this tick goes to the machines of
(NONE for every machine), and the machine of its owner */
static struct
{
	short player_count;
	short player_indices[MAXIMUM_EVENT_PLAYERS];
	long owner_machine_index;
} damage_event_destinations[MAXIMUM_DAMAGE_EVENTS_PER_TICK];
/* for the automated tests' reports (network_test.c) */
static long damage_rejected_reports;
static long damage_dealt_reports;
static long damage_sent_reports;
static long damage_replayed_events;

/* a client: its players' hits this tick */
static struct distributed_hit_report damage_reports[MAXIMUM_HIT_REPORTS_PER_TICK];
static short damage_report_count;
/* ... replaying the host's killing blow (its player effect came before),
and its killer and their score after it (NONE: none) */
static boolean damage_replaying_kill;
static long damage_replaying_killer;
static long damage_replaying_killer_score;

/* ---------- common */

word network_damage_entry_size(
	byte type)
{
	return type == _distributed_message_damage_events ?
		sizeof(struct distributed_damage_event) : sizeof(struct distributed_hit_report);
}

static void distributed_damage_from_data(
	struct damage_data const *damage,
	struct distributed_damage *result)
{
	csmemset(result, 0, sizeof(*result));
	result->definition_index = damage->definition_index;
	result->flags = damage->flags;
	result->owner_player_index = distributed_player_to_byte(damage->owner_player_index);
	result->owner_team_index = damage->owner_team_index;
	result->owner_object_index = damage->owner_object_index;
	result->origin = damage->origin;
	result->epicenter = damage->epicenter;
	result->direction = damage->direction;
	result->scale = damage->scale;
	result->multiplier = damage->multiplier;
	result->material_effect_scale = damage->material_effect_scale;
	result->material_type = damage->material_type;
}

/* the damage as this machine has it, FALSE if it cannot be */
static boolean distributed_damage_to_data(
	struct distributed_damage const *damage,
	struct damage_data *result)
{
	if (!tag_index_is_group(damage->definition_index, DAMAGE_EFFECT_DEFINITION_TAG))
		return FALSE;
	damage_data_new(result, damage->definition_index);
	result->flags = damage->flags;
	result->owner_player_index = distributed_player_from_byte(damage->owner_player_index);
	result->owner_team_index = damage->owner_team_index;
	result->owner_object_index = distributed_object_index_valid(damage->owner_object_index) &&
		object_try_and_get(damage->owner_object_index) ? damage->owner_object_index : NONE;
	result->origin = damage->origin;
	result->epicenter = damage->epicenter;
	result->direction = damage->direction;
	result->scale = damage->scale;
	result->multiplier = damage->multiplier;
	result->material_effect_scale = damage->material_effect_scale;
	result->material_type = damage->material_type;
	scenario_location_from_point(&result->location, &result->epicenter);
	return TRUE;
}

/* ---------- object_cause_damage (damage.c) */

/* whether the damage is a weapon's own at the unit that fires it (weapons.c:
no one's, from the weapon, at a point, the unit's centre) */
static boolean distributed_damage_is_recoil(
	struct damage_data const *damage,
	long unit_index)
{
	struct object_datum *unit = unit_index != NONE ?
		(struct object_datum *)object_try_and_get_and_verify_type(unit_index, _object_mask_unit) : NULL;

	return unit && damage->owner_player_index == NONE && damage->owner_object_index == NONE &&
		TEST_FLAG(damage->flags, _damage_from_weapon_bit) && !TEST_FLAG(damage->flags, _damage_area_of_effect_bit) &&
		damage->origin.x == damage->epicenter.x && damage->origin.y == damage->epicenter.y &&
		damage->origin.z == damage->epicenter.z && damage->epicenter.x == unit->object.bounding_sphere_center.x &&
		damage->epicenter.y == unit->object.bounding_sphere_center.y &&
		damage->epicenter.z == unit->object.bounding_sphere_center.z;
}

/* what the damage would show where this machine does not deal it, as
object_cause_damage has it (damage.c): the material it strikes, or the
shield's where the shield takes it, and how much is left of that, at the
first of the object and what it rides that takes it (a projectile responds
to it, projectiles.c; a client has the host's shields and health). */
static void distributed_damage_material(
	struct damage_data *damage,
	long object_index,
	short material_index)
{
	struct damage_effect_definition *definition = damage_effect_definition_get(damage->definition_index);
	long objects[16];
	short count = 0;
	short index;
	boolean parent_takes_body_damage = TRUE;
	real most = ((1.0f - damage->scale) * definition->damage.damage_minimum +
		definition->damage.damage_upper_bound * damage->scale) * damage->multiplier;

	/* (the object, and but for area damage what it rides, the outermost
	first) */
	while (object_index != NONE && count < NUMBEROF(objects))
	{
		objects[count++] = object_index;
		if (TEST_FLAG(damage->flags, _damage_area_of_effect_bit) || TEST_FLAG(damage->flags, _damage_kill_instantly_bit))
			break;
		object_index = object_get(object_index)->object.parent_object_index;
	}
	if (!count)
		return;
	{
		long collision_model_index = object_definition_get(object_get(objects[0])->definition_index)->
			object.collision_model.index;

		if (collision_model_index != NONE)
		{
			parent_takes_body_damage = !TEST_FLAG(collision_model_definition_get(collision_model_index)->resistance.flags,
				_damage_resistance_parent_never_takes_body_damage_for_us_bit);
		}
	}
	for (index = count - 1; index >= 0; index--)
	{
		struct object_datum *object = object_get(objects[index]);
		long collision_model_index = object_definition_get(object->definition_index)->object.collision_model.index;
		struct collision_model *collision_model;
		struct damage_resistance_material *material = NULL;
		boolean shield;
		boolean body;

		if (collision_model_index == NONE)
			continue;
		collision_model = collision_model_definition_get(collision_model_index);
		if (index == 0 && material_index >= 0 && material_index < collision_model->resistance.materials.count)
		{
			material = TAG_BLOCK_GET_ELEMENT(&collision_model->resistance.materials, material_index,
				struct damage_resistance_material);
		}
		else if (collision_model->resistance.indirect_damage_material_index >= 0 &&
			collision_model->resistance.indirect_damage_material_index < collision_model->resistance.materials.count)
		{
			material = TAG_BLOCK_GET_ELEMENT(&collision_model->resistance.materials,
				collision_model->resistance.indirect_damage_material_index, struct damage_resistance_material);
		}
		/* (none: damage.c's default material, all zero) */
		damage->material_type = material ? material->material_type : 0;
		shield = !TEST_FLAG(damage->flags, _damage_bypasses_shields_bit) &&
			!TEST_FLAG(definition->damage.flags, _damage_skips_shields_bit) &&
			object->object.maximum_shield_vitality > 0.0f && object->object.shield_vitality > 0.0f &&
			(!material || material->shield_leak_fraction < 1.0f) &&
			(index == 0 || TEST_FLAG(collision_model->resistance.flags,
				_damage_resistance_takes_shield_damage_for_children_bit));
		body = (index == 0 || (parent_takes_body_damage && TEST_FLAG(collision_model->resistance.flags,
			_damage_resistance_takes_body_damage_for_children_bit))) &&
			!TEST_FLAG(definition->damage.flags, _damage_only_hurts_shields_bit);
		if (most > 0.0f && (shield || (body &&
			(!TEST_FLAG(collision_model->resistance.flags, _damage_resistance_only_hurt_by_explosives_bit) ||
				TEST_FLAG(definition->damage.flags, _damage_detonates_explosives_bit)))))
		{
			if (shield)
			{
				damage->material_type = collision_model->resistance.shield_material_type;
				damage->material_effect_scale = object->object.shield_vitality;
			}
			else
			{
				damage->material_effect_scale = PIN(object->object.body_vitality, 0.0f, 1.0f);
			}
			return;
		}
		/* (the body of what it rides takes it, not the object's) */
		if (body)
			return;
	}
}

/* whether this machine deals the damage: a client none (it reports its own
players' hits instead), the host all but its clients' players' (but for
their reports); authorized: a client carrying out the host's word. Where it
does not, what the damage would show is filled in (the damage is
object_cause_damage's own). */
boolean network_damage_deals(
	struct damage_data const *damage,
	long object_index,
	short node_index,
	short region_index,
	short material_index,
	real_vector3d const *object_normal,
	boolean authorized)
{
	if (game_connection() == _game_connection_network_client)
	{
		struct unit_datum *unit;

		if (authorized)
			return TRUE;
		/* a weapon's own shake of this machine's own player firing it: its
		screen effect at once (the host does not send it,
		network_damage_player_effect), and nothing else of it */
		unit = (struct unit_datum *)object_try_and_get_and_verify_type(object_index, _object_mask_unit);
		if (unit && unit->unit.player_index != NONE && distributed_player_is_local(unit->unit.player_index) &&
			distributed_living_unit(player_try_and_get(unit->unit.player_index)) == object_index &&
			distributed_damage_is_recoil(damage, object_index))
		{
			struct damage_data effect = *damage;

			damage_replay_player_effect(unit->unit.player_index, &effect, 0.0f);
		}
		/* a hit of this machine's own player's, on the host's object */
		if (distributed_player_is_local(damage->owner_player_index) && network_objects_client_has(object_index) &&
			damage_report_count < MAXIMUM_HIT_REPORTS_PER_TICK)
		{
			struct distributed_hit_report *report = &damage_reports[damage_report_count++];

			csmemset(report, 0, sizeof(*report));
			report->object_index = object_index;
			distributed_damage_from_data(damage, &report->damage);
			/* (in the world: a rider's own position is its seat's) */
			object_get_origin(object_index, &report->target_position);
			report->node_index = node_index;
			report->region_index = region_index;
			report->material_index = material_index;
			report->host_time = distributed_latest_host_time();
			if (object_normal)
			{
				report->object_normal = *object_normal;
				report->has_normal = TRUE;
			}
		}
		distributed_damage_material((struct damage_data *)damage, object_index, material_index);
		return FALSE;
	}
	/* (a client's player's, whose machine reports it; but one whose machine
	has left deals it here: their grenades and rockets still in flight) */
	if (game_connection() == _game_connection_network_server && !damage_dealing_report &&
		damage->owner_player_index != NONE && !distributed_player_is_local(damage->owner_player_index))
	{
		struct player_datum *owner = player_try_and_get(damage->owner_player_index);

		if (owner && !owner->quit_out_of_game &&
			distributed_player_machine((short)DATUM_INDEX_TO_ABSOLUTE_INDEX(damage->owner_player_index)) != NONE)
		{
			distributed_damage_material((struct damage_data *)damage, object_index, material_index);
			return FALSE;
		}
	}
	return TRUE;
}

/* whether a client replaying the host's killing blow leaves out its
players' screen effects (which came as their own) */
boolean network_damage_replaying_kill(
	void)
{
	return damage_replaying_kill;
}

/* (game_engine.c) a client replaying the host's killing blow: its
killer's score after it, as the host's game type had it */
boolean network_damage_killer_score(
	long player_index,
	long *score)
{
	if (!damage_replaying_kill || damage_replaying_killer == NONE || player_index != damage_replaying_killer)
		return FALSE;
	*score = damage_replaying_killer_score;
	return TRUE;
}

/* the host: a player's screen shaken and flashed by damage */
void network_damage_player_effect(
	long player_index,
	struct damage_data const *damage,
	real total_damage)
{
	struct distributed_damage_event *event;

	/* (a weapon's own shake of the player firing it their machine shows
	itself, network_damage_deals) */
	if (game_connection() != _game_connection_network_server ||
		damage_event_count >= MAXIMUM_DAMAGE_EVENTS_PER_TICK - RESERVED_KILL_EVENTS ||
		distributed_player_is_local(player_index) ||
		distributed_damage_is_recoil(damage, distributed_living_unit(player_try_and_get(player_index))))
	{
		return;
	}
	event = &damage_events[damage_event_count++];
	csmemset(event, 0, sizeof(*event));
	event->kind = _damage_event_player_effect;
	event->player_index = distributed_player_to_byte(player_index);
	event->object_index = NONE;
	distributed_damage_from_data(damage, &event->damage);
	event->total_damage = total_damage;
}

/* the host: an object damaged, and what that did (object_damage_aftermath
done, so that a kill's killer is known) */
void network_damage_aftermath(
	long object_index,
	struct damage_data const *damage,
	unsigned long being_damaged_flags,
	real shield_damage,
	real body_damage,
	real body_damage_multiplier,
	short body_part,
	short node_index,
	short region_index,
	short material_index,
	long victim_player_index)
{
	struct distributed_damage_event *event;
	struct unit_datum *unit;
	boolean kill;

	if (game_connection() != _game_connection_network_server)
		return;
	/* (units only: items and the like the objects' states place) */
	unit = (struct unit_datum *)object_try_and_get_and_verify_type(object_index, _object_mask_unit);
	if (!unit)
		return;
	/* (the last events kept for killing blows: of a player's unit, whose
	player the blow's aftermath has already taken from it, unit_died; or of
	a biped no player's that was alive, an actor's: a body gibbed, or a
	vehicle, whose riders the blow would kill again where it is replayed,
	goes as other damage does) */
	kill = TEST_FLAG(being_damaged_flags, _object_being_damaged_body_depleted_bit) &&
		(victim_player_index != NONE ||
			(TEST_FLAG(_object_mask_biped, unit->object.type) &&
				!TEST_FLAG(being_damaged_flags, _object_being_damaged_body_destroyed_bit)));
	if (damage_event_count >= MAXIMUM_DAMAGE_EVENTS_PER_TICK - (kill ? 0 : RESERVED_KILL_EVENTS))
		return;
	event = &damage_events[damage_event_count++];
	csmemset(event, 0, sizeof(*event));
	event->kind = _damage_event_aftermath;
	event->player_index = NO_PLAYER;
	event->object_index = object_index;
	distributed_damage_from_data(damage, &event->damage);
	event->being_damaged_flags = being_damaged_flags;
	event->shield_damage = shield_damage;
	event->body_damage = body_damage;
	event->body_damage_multiplier = body_damage_multiplier;
	event->body_part = body_part;
	event->node_index = node_index;
	event->region_index = region_index;
	event->material_index = material_index;
	/* a killing blow, a player's with who the host says dealt it */
	if (kill)
	{
		boolean friendly_fire = FALSE;
		boolean killed_by_vehicle = FALSE;

		event->kind = _damage_event_kill;
		/* (no killer when the game noted none: a death it did not score, or
		no player's) */
		if (victim_player_index == NONE ||
			!distributed_get_death((short)DATUM_INDEX_TO_ABSOLUTE_INDEX(victim_player_index),
				&event->player_index, &friendly_fire, &killed_by_vehicle))
		{
			event->player_index = NO_PLAYER;
			friendly_fire = FALSE;
			killed_by_vehicle = FALSE;
		}
		SET_FLAG(event->kill_flags, _damage_event_friendly_fire_bit, friendly_fire);
		SET_FLAG(event->kill_flags, _damage_event_killed_by_vehicle_bit, killed_by_vehicle);
		SET_FLAG(event->kill_flags, _damage_event_telefragged_bit,
			victim_player_index != NONE && player_get(victim_player_index)->telefrag_timeout >= 90);
		if (event->player_index != NO_PLAYER)
		{
			long killer = distributed_player_from_byte(event->player_index);

			if (killer != NONE)
				event->killer_score = game_engine_network_player_score(killer);
		}
	}
}

/* ---------- the host */

/* the reach, the farther of it and the other (unbounded if either is) */
static void distributed_reach_combine(
	struct damage_reach *reach,
	struct damage_reach const *other)
{
	if (!(reach->distance >= 0.0f) || !(other->distance >= 0.0f))
	{
		reach->distance = -1.0f;
		return;
	}
	reach->distance = MAX(reach->distance, other->distance);
	reach->ticks = MAX(reach->ticks, other->ticks);
	reach->carried_ticks = MAX(reach->carried_ticks, other->carried_ticks);
}

/* how far a projectile flies before it goes off or is gone, and how long
it takes (projectiles.c): its range (its path no longer), and no longer than
it takes at its slowest or its timer, when that starts at once; else as far
as it goes at its fastest for as long as its timer runs (and falls, and is
carried at its firer's speed). Unbounded where it has neither, where its
timer starts once it bounces or rests, and where it sticks to what it hits,
which carries it. */
static void distributed_projectile_flight(
	struct projectile_definition const *projectile,
	struct damage_reach *flight)
{
	real fastest = MAX(projectile->projectile.initial_velocity, projectile->projectile.final_velocity);
	real slowest = MIN(projectile->projectile.initial_velocity, projectile->projectile.final_velocity);
	real timer_ticks = projectile->projectile.detonation_timer_starts == _projectile_detonation_timer_starts_immediately ?
		projectile->projectile.timer_upper_bound * TICKS_PER_SECOND : 0.0f;
	short response_index;

	flight->distance = -1.0f;
	flight->ticks = (real)PLAYER_TRAIL_TICKS;
	flight->carried_ticks = 0.0f;
	for (response_index = 0; response_index < projectile->projectile.material_responses.count; response_index++)
	{
		struct projectile_material_response_definition *response = TAG_BLOCK_GET_ELEMENT(
			&projectile->projectile.material_responses, response_index,
			struct projectile_material_response_definition);

		if (response->default_response == _projectile_material_response_attach ||
			response->potential_response == _projectile_material_response_attach)
		{
			return;
		}
	}
	/* (so written that a tag's number not a number is no bound) */
	if (projectile->projectile.maximum_range > 0.0f)
	{
		flight->distance = projectile->projectile.maximum_range;
		/* (twice as long: a lob's arc) */
		if (slowest > 0.0f && 2.0f * projectile->projectile.maximum_range / slowest < flight->ticks)
			flight->ticks = 2.0f * projectile->projectile.maximum_range / slowest;
		if (timer_ticks > 0.0f && timer_ticks < flight->ticks)
			flight->ticks = timer_ticks;
	}
	else if (timer_ticks > 0.0f && fastest >= 0.0f)
	{
		real gravity = global_gravity * MAX(0.0f, MAX(projectile->projectile.air_gravity_scale,
			projectile->projectile.water_gravity_scale));

		flight->distance = fastest * timer_ticks + 0.5f * gravity * timer_ticks * timer_ticks;
		flight->ticks = MIN(timer_ticks, (real)PLAYER_TRAIL_TICKS);
		flight->carried_ticks = timer_ticks;
		if (!(flight->distance >= 0.0f))
			flight->distance = -1.0f;
	}
}

/* how a projectile's impacts and detonations deal the damage (the
_damage_source flags, none for not at all), and how far from its firer
(combined into reach) */
static byte distributed_projectile_deals(long projectile_index, long damage_index, short depth,
	struct damage_reach *reach);

/* ... and an effect's: its own damage as an area's (effects.c), where it
goes off, its projectiles' as theirs, from there */
static byte distributed_effect_deals(
	long effect_index,
	long damage_index,
	short depth,
	struct damage_reach *reach)
{
	struct effect_definition *effect;
	short event_index;
	byte kinds = 0;

	if (depth > MAXIMUM_TAG_DEPTH || !tag_index_is_group(effect_index, EFFECT_DEFINITION_TAG))
		return 0;
	effect = (struct effect_definition *)tag_get(EFFECT_DEFINITION_TAG, effect_index);
	for (event_index = 0; event_index < effect->events.count; event_index++)
	{
		struct effect_event_definition *event = TAG_BLOCK_GET_ELEMENT(
			&effect->events, event_index, struct effect_event_definition);
		short part_index;

		for (part_index = 0; part_index < event->parts.count; part_index++)
		{
			struct effect_part_definition *part = TAG_BLOCK_GET_ELEMENT(
				&event->parts, part_index, struct effect_part_definition);

			if (part->reference.index == NONE)
				continue;
			if (part->reference.index == damage_index)
				SET_FLAG(kinds, _damage_source_area_bit, TRUE);
			else if (part->reference.group_tag == PROJECTILE_DEFINITION_TAG)
				kinds |= distributed_projectile_deals(part->reference.index, damage_index, depth + 1, reach);
			else if (part->reference.group_tag == EFFECT_DEFINITION_TAG)
				kinds |= distributed_effect_deals(part->reference.index, damage_index, depth + 1, reach);
		}
	}
	return kinds;
}

static byte distributed_projectile_deals(
	long projectile_index,
	long damage_index,
	short depth,
	struct damage_reach *reach)
{
	struct projectile_definition *projectile;
	struct damage_reach from_here = {0.0f, 0.0f, 0.0f};
	short response_index;
	byte kinds = 0;

	if (depth > MAXIMUM_TAG_DEPTH || !tag_index_is_group(projectile_index, PROJECTILE_DEFINITION_TAG))
		return 0;
	projectile = projectile_definition_get(projectile_index);
	/* (its impact, and its detonation on what it sticks to, at a point,
	projectiles.c; its effect is the one it detonates with: a grenade's, a
	rocket's explosion) */
	if (projectile->projectile.impact_damage.index == damage_index ||
		projectile->projectile.attached_detonation_damage.index == damage_index)
	{
		SET_FLAG(kinds, _damage_source_impact_bit, TRUE);
	}
	kinds |= distributed_effect_deals(projectile->projectile.effect.index, damage_index, depth + 1, &from_here);
	kinds |= distributed_effect_deals(projectile->projectile.super_detonation.index, damage_index, depth + 1,
		&from_here);
	kinds |= distributed_effect_deals(projectile->projectile.detonation_started.index, damage_index, depth + 1,
		&from_here);
	for (response_index = 0; response_index < projectile->projectile.material_responses.count; response_index++)
	{
		struct projectile_material_response_definition *response = TAG_BLOCK_GET_ELEMENT(
			&projectile->projectile.material_responses, response_index,
			struct projectile_material_response_definition);

		kinds |= distributed_effect_deals(response->default_effect.index, damage_index, depth + 1, &from_here);
		kinds |= distributed_effect_deals(response->potential_effect.index, damage_index, depth + 1, &from_here);
		kinds |= distributed_effect_deals(response->detonation_effect.index, damage_index, depth + 1, &from_here);
	}
	/* (from its firer: its flight, then what it deals from where it goes
	off) */
	if (kinds)
	{
		struct damage_reach flight;

		distributed_projectile_flight(projectile, &flight);
		if (!(flight.distance >= 0.0f) || !(from_here.distance >= 0.0f))
		{
			from_here.distance = -1.0f;
		}
		else
		{
			from_here.distance += flight.distance;
			from_here.ticks += flight.ticks;
			from_here.carried_ticks += flight.carried_ticks;
		}
		distributed_reach_combine(reach, &from_here);
	}
	return kinds;
}

/* how many hits a second the weapon deals of the damage, 0 for none; how it
deals it (the _damage_source flags); and how far from its firer its
projectiles do (combined into reach; a weapon's own detonation goes off
where the weapon is, unbounded) */
static real distributed_weapon_deals(
	long weapon_definition_index,
	long damage_index,
	byte *kinds,
	struct damage_reach *reach)
{
	struct weapon_definition *weapon;
	short trigger_index;
	byte effect_kinds;
	real rate = 0.0f;

	*kinds = 0;
	if (!tag_index_is_group(weapon_definition_index, WEAPON_DEFINITION_TAG))
		return 0.0f;
	weapon = weapon_definition_get(weapon_definition_index);
	if (weapon->weapon.melee_attack_damage.index == damage_index)
	{
		rate = MELEE_HITS_PER_SECOND;
		SET_FLAG(*kinds, _damage_source_melee_bit, TRUE);
	}
	{
		struct damage_reach weapon_reach = {0.0f, 0.0f, 0.0f};

		effect_kinds = distributed_effect_deals(weapon->weapon.detonation_effect.index, damage_index, 0, &weapon_reach) |
			distributed_effect_deals(weapon->weapon.overheated_effect.index, damage_index, 0, &weapon_reach);
	}
	if (effect_kinds)
	{
		rate = MAX(rate, EFFECT_HITS_PER_SECOND);
		*kinds |= effect_kinds;
		reach->distance = -1.0f;
	}
	for (trigger_index = 0; trigger_index < weapon->weapon.triggers.count; trigger_index++)
	{
		struct weapon_trigger_definition *trigger = TAG_BLOCK_GET_ELEMENT(
			&weapon->weapon.triggers, trigger_index, struct weapon_trigger_definition);
		byte projectile_kinds = distributed_projectile_deals(trigger->projectile.index, damage_index, 0, reach);

		if (projectile_kinds)
		{
			real rate_of_fire = MAX(trigger->initial_rate_of_fire, trigger->final_rate_of_fire);

			/* (none is no limit, weapon_trigger_can_fire_again: a shot a tick,
			or a press, the plasma pistol's, as fast as a player taps) */
			if (rate_of_fire <= 0.0001f)
			{
				rate_of_fire = TEST_FLAG(trigger->flags, _weapon_trigger_latched_bit) ?
					TICKS_PER_SECOND / 2.0f : (real)TICKS_PER_SECOND;
			}
			rate = MAX(rate, MAX(rate_of_fire, 1.0f) * MAX(trigger->projectiles_per_shot, 1));
			*kinds |= projectile_kinds;
		}
	}
	return rate;
}

/* the source's (a weapon's, a grenade's projectile's) hits a second of the
damage, how it deals it (the _damage_source flags), and how far from its
firer, from its tags */
static real distributed_source_deals_uncached(
	long source_index,
	long damage_index,
	boolean grenade,
	byte *kinds,
	struct damage_reach *reach)
{
	reach->distance = 0.0f;
	reach->ticks = 0.0f;
	reach->carried_ticks = 0.0f;
	if (grenade)
	{
		*kinds = distributed_projectile_deals(source_index, damage_index, 0, reach);
		return *kinds ? (real)GRENADE_HITS_PER_SECOND : 0.0f;
	}
	return distributed_weapon_deals(source_index, damage_index, kinds, reach);
}

/* ... walking its tags once a game */
static real distributed_source_deals(
	long source_index,
	long damage_index,
	boolean grenade,
	byte *kinds,
	struct damage_reach *reach)
{
	unsigned long hash = ((unsigned long)source_index * 2654435761u) ^ ((unsigned long)damage_index * 40503u);
	short probe;

	for (probe = 0; probe < 16; probe++)
	{
		struct damage_rate_cache_entry *entry =
			&damage_rate_cache[(hash + probe) & (DAMAGE_RATE_CACHE_SIZE - 1)];

		if (entry->source_index == source_index && entry->damage_index == damage_index)
		{
			*kinds = entry->kinds;
			*reach = entry->reach;
			return entry->rate;
		}
		if (entry->source_index == NONE)
		{
			entry->source_index = source_index;
			entry->damage_index = damage_index;
			entry->rate = distributed_source_deals_uncached(source_index, damage_index, grenade, &entry->kinds,
				&entry->reach);
			*kinds = entry->kinds;
			*reach = entry->reach;
			return entry->rate;
		}
	}
	return distributed_source_deals_uncached(source_index, damage_index, grenade, kinds, reach);
}

/* how many hits a second the player could deal of the damage: their
weapons, lately, their grenades, the vehicle they drove lately; 0 when none
of them deals it; how they deal it (the _damage_source flags); and how far
from them what they fire deals it */
static real distributed_player_deals(
	short player_index,
	long damage_index,
	byte *kinds,
	struct damage_reach *reach)
{
	struct game_globals *globals = scenario_get_game_globals();
	short index;
	real rate = 0.0f;

	*kinds = 0;
	reach->distance = 0.0f;
	reach->ticks = 0.0f;
	reach->carried_ticks = 0.0f;
	for (index = 0; index < MAXIMUM_RECENT_WEAPONS; index++)
	{
		if (damage_players[player_index].definition_indices[index] != NONE &&
			game_time_get() - damage_players[player_index].times[index] <= RECENT_WEAPON_TICKS)
		{
			byte weapon_kinds;
			struct damage_reach weapon_reach;
			real weapon_rate = distributed_source_deals(damage_players[player_index].definition_indices[index],
				damage_index, FALSE, &weapon_kinds, &weapon_reach);

			if (weapon_rate > 0.0f)
				distributed_reach_combine(reach, &weapon_reach);
			rate = MAX(rate, weapon_rate);
			*kinds |= weapon_kinds;
		}
	}
	for (index = 0; index < globals->grenades.count && index < NUMBER_OF_UNIT_GRENADE_TYPES; index++)
	{
		struct game_globals_grenade *grenade = TAG_BLOCK_GET_ELEMENT(&globals->grenades, index,
			struct game_globals_grenade);

		if (game_time_get() - damage_players[player_index].grenade_times[index] <= RECENT_WEAPON_TICKS &&
			grenade->projectile.index != NONE)
		{
			byte grenade_kinds;
			struct damage_reach grenade_reach;
			real grenade_rate = distributed_source_deals(grenade->projectile.index, damage_index, TRUE, &grenade_kinds,
				&grenade_reach);

			if (grenade_rate > 0.0f)
				distributed_reach_combine(reach, &grenade_reach);
			rate = MAX(rate, grenade_rate);
			*kinds |= grenade_kinds;
		}
	}
	/* (a blow with no weapon: the unit's own, units.c) */
	if (damage_players[player_index].unarmed_melee_damage_index == damage_index && damage_index != NONE &&
		game_time_get() - damage_players[player_index].unarmed_time <= RECENT_WEAPON_TICKS)
	{
		rate = MAX(rate, MELEE_HITS_PER_SECOND);
		SET_FLAG(*kinds, _damage_source_melee_bit, TRUE);
	}
	/* (a vehicle's collision is its driver's, physics.c; the damage it deals
	a unit it kills is no one's) */
	if (game_time_get() - damage_players[player_index].driven_time <= RECENT_WEAPON_TICKS &&
		globals->falling_damage.count > 0)
	{
		struct distributed_falling_damage *falling_damage = TAG_BLOCK_GET_ELEMENT(&globals->falling_damage, 0,
			struct distributed_falling_damage);

		if (falling_damage->vehicle_collision_damage.index == damage_index)
		{
			rate = MAX(rate, VEHICLE_HITS_PER_SECOND);
			SET_FLAG(*kinds, _damage_source_collision_bit, TRUE);
		}
	}
	return rate * HIT_REPORT_RATE_MARGIN;
}

/* the distance between the points, squared */
static real distributed_distance_squared(
	real_point3d const *a,
	real_point3d const *b)
{
	real dx = a->x - b->x;
	real dy = a->y - b->y;
	real dz = a->z - b->z;

	return dx * dx + dy * dy + dz * dz;
}

/* where each player's unit and vehicle are this tick (and whether they
drive it), noted; and every few ticks where their unit is */
static void distributed_note_targets(
	void)
{
	struct damage_history_tick *tick = &damage_history[game_time_get() & (TARGET_HISTORY_TICKS - 1)];
	short player_index;

	tick->time = game_time_get();
	for (player_index = 0; player_index < MAXIMUM_TRACKED_PLAYERS; player_index++)
	{
		long unit_index = distributed_living_unit(distributed_player(player_index));
		short slot;

		tick->objects[player_index][0].object_index = unit_index;
		tick->objects[player_index][1].object_index = unit_index != NONE ?
			object_get(unit_index)->object.parent_object_index : NONE;
		tick->objects[player_index][0].driving = FALSE;
		tick->objects[player_index][1].driving = FALSE;
		for (slot = 0; slot < 2; slot++)
		{
			long object_index = tick->objects[player_index][slot].object_index;
			real_vector3d velocity;

			if (object_index == NONE)
				continue;
			object_get_origin(object_index, &tick->objects[player_index][slot].position);
			/* (a rider's own velocity is not kept: what it rides has it) */
			object_get_velocities(object_index, &velocity, NULL);
			tick->objects[player_index][slot].speed = (real)sqrt(velocity.i * velocity.i + velocity.j * velocity.j +
				velocity.k * velocity.k);
		}
		if (tick->objects[player_index][1].object_index != NONE)
		{
			struct unit_datum *vehicle = (struct unit_datum *)object_try_and_get_and_verify_type(
				tick->objects[player_index][1].object_index, _object_mask_vehicle);

			tick->objects[player_index][1].driving = vehicle && vehicle->unit.driver_object_index == unit_index;
		}
		if (game_time_get() % PLAYER_TRAIL_INTERVAL_TICKS == 0)
		{
			struct damage_trail_point *point =
				&damage_trails[player_index][(game_time_get() / PLAYER_TRAIL_INTERVAL_TICKS) & (PLAYER_TRAIL_POINTS - 1)];

			point->time = unit_index != NONE ? game_time_get() : NONE;
			if (unit_index != NONE)
			{
				point->position = tick->objects[player_index][0].position;
				point->speed = tick->objects[player_index][0].speed;
				point->radius = 0.0f;
				if (tick->objects[player_index][1].object_index != NONE)
				{
					point->speed = MAX(point->speed, tick->objects[player_index][1].speed);
					point->radius = object_get(tick->objects[player_index][1].object_index)->object.bounding_sphere_radius;
				}
			}
		}
	}
}

/* whether the host had the history's object within the tolerance of the
position (with how far it moves in a few ticks) */
static boolean distributed_history_near(
	real_point3d const *history_position,
	real speed,
	real_point3d const *position,
	real tolerance)
{
	real reach = tolerance + REPORT_HISTORY_LEAD_TICKS * speed;

	return distributed_distance_squared(position, history_position) <= reach * reach;
}

/* whether the host had the object (a player's unit or vehicle) within
reach of the position at a tick in the last ticks; NONE when it has no such
object now (not a player's: where it is now is what counts). Each tick is
looked through only where the tick after had it (a player's unit, or their
vehicle), unless it is not there. */
static short distributed_target_seen(
	long object_index,
	real_point3d const *position,
	long ticks)
{
	boolean latest = FALSE;
	short column_player_index = NONE;
	short column_slot = 0;
	long back;

	for (back = 0; back <= ticks && back < TARGET_HISTORY_TICKS; back++)
	{
		long time = game_time_get() - back;
		struct damage_history_tick const *tick = &damage_history[time & (TARGET_HISTORY_TICKS - 1)];

		if (tick->time != time)
			break;
		if (column_player_index == NONE ||
			tick->objects[column_player_index][column_slot].object_index != object_index)
		{
			short player_index;

			column_player_index = NONE;
			for (player_index = 0; player_index < MAXIMUM_TRACKED_PLAYERS && column_player_index == NONE;
				player_index++)
			{
				short slot;

				for (slot = 0; slot < 2; slot++)
				{
					if (tick->objects[player_index][slot].object_index == object_index)
					{
						column_player_index = player_index;
						column_slot = slot;
						break;
					}
				}
			}
			if (column_player_index == NONE)
				continue;
		}
		if (back == 0)
			latest = TRUE;
		if (distributed_history_near(&tick->objects[column_player_index][column_slot].position,
			tick->objects[column_player_index][column_slot].speed, position, REPORT_HISTORY_TOLERANCE))
		{
			return TRUE;
		}
	}
	/* (one that was a player's, a body, a vehicle left: where it is now) */
	return latest ? FALSE : NONE;
}

/* whether the host had the player's unit within reach of the position at a
tick in the last ticks */
static boolean distributed_player_was_near(
	short player_index,
	real_point3d const *position,
	long ticks)
{
	long back;

	for (back = 0; back <= ticks && back < TARGET_HISTORY_TICKS; back++)
	{
		long time = game_time_get() - back;
		struct damage_history_tick const *tick = &damage_history[time & (TARGET_HISTORY_TICKS - 1)];

		if (tick->time != time)
			break;
		if (tick->objects[player_index][0].object_index != NONE &&
			distributed_history_near(&tick->objects[player_index][0].position, tick->objects[player_index][0].speed,
				position, REPORT_HISTORY_TOLERANCE))
		{
			return TRUE;
		}
	}
	return FALSE;
}

/* whether the host had the player driving a vehicle at a tick in the last
ticks within reach of the epicenter (its size and REPORT_VEHICLE_TOLERANCE
more), and the target (where the report has it) within the vehicle's reach
of the epicenter: their sizes and the push the vehicle gives it
(physics.c) */
static boolean distributed_player_drove_into(
	short player_index,
	real_point3d const *epicenter,
	real_point3d const *target_position,
	real target_radius,
	long ticks)
{
	long back;

	for (back = 0; back <= ticks && back < TARGET_HISTORY_TICKS; back++)
	{
		long time = game_time_get() - back;
		struct damage_history_tick const *tick = &damage_history[time & (TARGET_HISTORY_TICKS - 1)];
		struct object_datum *vehicle;
		real reach;

		if (tick->time != time)
			break;
		if (tick->objects[player_index][1].object_index == NONE || !tick->objects[player_index][1].driving ||
			!(vehicle = object_try_and_get(tick->objects[player_index][1].object_index)))
		{
			continue;
		}
		reach = target_radius + vehicle->object.bounding_sphere_radius + REPORT_IMPACT_TOLERANCE +
			REPORT_COLLISION_LEAD_TICKS * tick->objects[player_index][1].speed;
		if (distributed_history_near(&tick->objects[player_index][1].position, tick->objects[player_index][1].speed,
				epicenter, REPORT_HISTORY_TOLERANCE + vehicle->object.bounding_sphere_radius + REPORT_VEHICLE_TOLERANCE) &&
			distributed_distance_squared(epicenter, target_position) <= reach * reach)
		{
			return TRUE;
		}
	}
	return FALSE;
}

/* whether the host had the player (their living unit, or the vehicle it
rode) within reach of the point since what they fired could have been:
the reach's ticks before the host's tick the report was made at, and a
little more (the whole trail for more); with how far they go in the ticks
between two of the trail's and a few more (the report may come before the
host has the player where they fired from) */
static boolean distributed_player_could_fire(
	long machine_index,
	short player_index,
	real_point3d const *point,
	struct damage_reach const *reach,
	long host_time)
{
	real ticks = reach->ticks;
	long earliest;
	long latest_made = game_time_get() - (long)distributed_machine_round_trip_ticks(machine_index) -
		PLAYER_TRAIL_REPORT_SLACK_TICKS;
	short index;

	if (host_time < latest_made)
		host_time = latest_made;
	if (!(ticks <= (real)PLAYER_TRAIL_TICKS))
		ticks = (real)PLAYER_TRAIL_TICKS;
	earliest = host_time - (long)ticks - TARGET_HISTORY_SLACK_TICKS - PLAYER_TRAIL_INTERVAL_TICKS;
	for (index = 0; index < PLAYER_TRAIL_POINTS; index++)
	{
		struct damage_trail_point const *trail_point = &damage_trails[player_index][index];
		real distance;

		if (trail_point->time == NONE || trail_point->time < earliest || trail_point->time > game_time_get())
			continue;
		distance = reach->distance + REPORT_RANGE_TOLERANCE + trail_point->radius + trail_point->speed *
			(reach->carried_ticks + PLAYER_TRAIL_INTERVAL_TICKS + TARGET_HISTORY_SLACK_TICKS);
		if (distributed_distance_squared(&trail_point->position, point) <= distance * distance)
			return TRUE;
	}
	return FALSE;
}

/* the weapons each player carries, and whether they drive, noted */
static void distributed_note_weapons(
	void)
{
	struct data_iterator iterator;
	struct player_datum *player;

	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		short player_index = (short)DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.datum_index);
		long unit_index = distributed_living_unit(player);
		long units[2];
		short unit_number;

		if (player_index >= MAXIMUM_TRACKED_PLAYERS || unit_index == NONE)
			continue;
		damage_players[player_index].unarmed_melee_damage_index = unit_unarmed_melee_damage(unit_index);
		damage_players[player_index].unarmed_time = game_time_get();
		units[0] = unit_index;
		units[1] = object_get(unit_index)->object.parent_object_index;
		/* (a vehicle's collisions are its driver's, physics.c) */
		{
			struct unit_datum *vehicle = units[1] != NONE ?
				(struct unit_datum *)object_try_and_get_and_verify_type(units[1], _object_mask_vehicle) : NULL;

			if (vehicle && vehicle->unit.driver_object_index == unit_index)
				damage_players[player_index].driven_time = game_time_get();
		}
		{
			struct unit_datum *unit = unit_get(unit_index);
			short grenade_index;

			for (grenade_index = 0; grenade_index < NUMBER_OF_UNIT_GRENADE_TYPES; grenade_index++)
			{
				if (unit->unit.grenade_counts[grenade_index] > 0)
					damage_players[player_index].grenade_times[grenade_index] = game_time_get();
			}
		}
		for (unit_number = 0; unit_number < 2; unit_number++)
		{
			struct unit_datum *unit = units[unit_number] != NONE ?
				(struct unit_datum *)object_try_and_get_and_verify_type(units[unit_number], _object_mask_unit) : NULL;
			short weapon_slot;

			if (!unit)
				continue;
			/* (a vehicle's weapons its driver's and gunner's, not its
			passengers') */
			if (unit_number == 1 && unit->unit.driver_object_index != unit_index &&
				unit->unit.gunner_object_index != unit_index)
			{
				continue;
			}
			for (weapon_slot = 0; weapon_slot < MAXIMUM_WEAPONS_PER_UNIT; weapon_slot++)
			{
				long weapon_index = unit->unit.weapon_object_indices[weapon_slot];
				long definition_index;
				short index;
				short oldest = 0;

				if (weapon_index == NONE || !object_try_and_get(weapon_index))
					continue;
				definition_index = object_get(weapon_index)->definition_index;
				for (index = 0; index < MAXIMUM_RECENT_WEAPONS; index++)
				{
					if (damage_players[player_index].definition_indices[index] == definition_index)
						break;
					if (damage_players[player_index].times[index] < damage_players[player_index].times[oldest])
						oldest = index;
				}
				if (index == MAXIMUM_RECENT_WEAPONS)
				{
					index = oldest;
					damage_players[player_index].definition_indices[index] = definition_index;
				}
				damage_players[player_index].times[index] = game_time_get();
			}
		}
	}
}

/* whether the node, region and material are the object's (or NONE) */
static boolean distributed_damage_indices_valid(
	long object_index,
	short node_index,
	short region_index,
	short material_index)
{
	struct object_datum *object = object_get(object_index);
	long collision_model_index = object_definition_get(object->definition_index)->object.collision_model.index;
	struct collision_model *collision_model = collision_model_index != NONE ?
		collision_model_definition_get(collision_model_index) : NULL;

	if (node_index != NONE && (node_index < 0 || !object_has_node(object_index, node_index)))
		return FALSE;
	if (region_index != NONE && (region_index < 0 || region_index >= MAXIMUM_REGIONS_PER_OBJECT ||
		!collision_model || region_index >= collision_model->resistance.regions.count))
	{
		return FALSE;
	}
	if (material_index != NONE && (material_index < 0 || !collision_model ||
		material_index >= collision_model->resistance.materials.count))
	{
		return FALSE;
	}
	return TRUE;
}

/* whether the damage's numbers are all finite and in the world */
static boolean distributed_damage_numbers_valid(
	struct distributed_damage const *damage)
{
	return distributed_point_valid(&damage->origin, REPORT_WORLD_BOUND) &&
		distributed_point_valid(&damage->epicenter, REPORT_WORLD_BOUND) &&
		distributed_point_valid((real_point3d const *)&damage->direction, 2.0f) &&
		distributed_real_valid(damage->scale) && distributed_real_valid(damage->multiplier) &&
		distributed_real_valid(damage->material_effect_scale);
}

/* whether the object is the player's: their living unit (unit_index), the
vehicle it rides, or a unit of theirs (a grenade outlives its thrower) */
static boolean distributed_object_is_players(
	short player_index,
	long unit_index,
	long object_index)
{
	if (object_index == NONE)
		return FALSE;
	if (object_index == unit_index ||
		(unit_index != NONE && object_index == object_get(unit_index)->object.parent_object_index))
	{
		return TRUE;
	}
	/* (their unit of a life before, its body: a grenade, a rocket, outlives
	its thrower, player_died) */
	{
		struct player_datum *player = distributed_player(player_index);

		return player && player->dead_unit_index != NONE && object_index == player->dead_unit_index;
	}
}

/* whether the player has hits left for the report, which it takes (at the
weapon's rate). An explosion's hits of a tick are one: a report of the same
explosion as another of that player's that tick (its damage and epicenter)
is free, when the damage has a reach (a cutoff radius), but no object is hit
twice by one. */
static boolean distributed_report_paid(
	short player_index,
	struct distributed_hit_report const *report,
	real rate,
	boolean explosion,
	boolean reach)
{
	real *hit_seconds = &damage_players[player_index].hit_seconds;
	long elapsed = game_time_get() - damage_players[player_index].hit_seconds_time;
	boolean paid = FALSE;
	short index;

	if (damage_explosions[player_index].time != game_time_get())
	{
		damage_explosions[player_index].time = game_time_get();
		damage_explosions[player_index].count = 0;
	}
	if (explosion)
	{
		for (index = 0; index < damage_explosions[player_index].count; index++)
		{
			if (damage_explosions[player_index].explosions[index].definition_index ==
					report->damage.definition_index &&
				damage_explosions[player_index].explosions[index].epicenter.x == report->damage.epicenter.x &&
				damage_explosions[player_index].explosions[index].epicenter.y == report->damage.epicenter.y &&
				damage_explosions[player_index].explosions[index].epicenter.z == report->damage.epicenter.z)
			{
				if (damage_explosions[player_index].explosions[index].object_index == report->object_index)
					return FALSE;
				paid = reach;
			}
		}
		/* (free only when noted: each object once) */
		if (damage_explosions[player_index].count >= MAXIMUM_EXPLOSIONS_PER_TICK)
			paid = FALSE;
	}
	if (!paid)
	{
		*hit_seconds = MIN((real)HIT_REPORT_BURST_SECONDS, *hit_seconds + (real)elapsed / TICKS_PER_SECOND);
		damage_players[player_index].hit_seconds_time = game_time_get();
		if (*hit_seconds < 1.0f / rate)
			return FALSE;
		*hit_seconds -= 1.0f / rate;
	}
	if (explosion && damage_explosions[player_index].count < MAXIMUM_EXPLOSIONS_PER_TICK)
	{
		short count = damage_explosions[player_index].count++;

		damage_explosions[player_index].explosions[count].definition_index = report->damage.definition_index;
		damage_explosions[player_index].explosions[count].epicenter = report->damage.epicenter;
		damage_explosions[player_index].explosions[count].object_index = report->object_index;
	}
	return TRUE;
}

/* whether the host takes the report, which it makes the damage as the
host deals it (the cheaper checks first, and the report paid for before the
history is looked through: a flood of reports costs its sender no more than
the hits it pays for). The report is of the shape the game gives the damage
(_damage_source flags): what hits at a point there (its origin the
epicenter), a melee blow from the striker (its origin their head, its
epicenter their body, both where the host had them, and the target within
their reach), a vehicle's collision from the vehicle its driver drove (its
epicenter the vehicle, where the host had it, the target at it); its origin
at the target in each, and what hits at a point within its reach of where
the host had the player since it was fired; and area damage or not as the
game deals it, an explosion's direction and scale from the epicenter to the
target. */
static boolean distributed_report_valid(
	long machine_index,
	struct distributed_hit_report const *report,
	struct damage_data *damage)
{
	short player_index = report->damage.owner_player_index;
	long now = game_time_get();
	struct player_datum *player;
	struct object_datum *target;
	struct damage_effect_definition *definition;
	long ticks;
	long unit_index;
	byte kinds;
	boolean melee = FALSE;
	boolean collision = FALSE;
	boolean area;
	real rate;
	real reach;
	struct damage_reach fired_reach;
	short seen;

	/* that machine's player */
	if (player_index == NO_PLAYER || player_index >= MAXIMUM_TRACKED_PLAYERS ||
		!(player = distributed_player(player_index)) || !distributed_machine_has_player(machine_index, player_index))
	{
		return FALSE;
	}
	/* one of the host's objects (the index whole: identifier 0 is any
	object at the index) */
	if (!distributed_object_index_valid(report->object_index))
		return FALSE;
	target = (struct object_datum *)object_try_and_get_and_verify_type(report->object_index,
		_object_mask_biped | _object_mask_vehicle | _object_mask_weapon | _object_mask_equipment);
	if (!target || !tag_index_is_group(report->damage.definition_index, DAMAGE_EFFECT_DEFINITION_TAG))
		return FALSE;
	/* numbers the game can take */
	if (!distributed_damage_numbers_valid(&report->damage) ||
		!distributed_point_valid(&report->target_position, REPORT_WORLD_BOUND) ||
		(report->has_normal && !distributed_point_valid((real_point3d const *)&report->object_normal, 2.0f)) ||
		!distributed_damage_indices_valid(report->object_index, report->node_index, report->region_index,
			report->material_index))
	{
		return FALSE;
	}
	/* made at a tick of the host's not later than now nor long before (one
	held back is refused, as a burst after the network was lost is), and
	looked back over from then: however long it took to come (sent again,
	reliably), where the shooter saw the target */
	if (report->host_time == NONE || report->host_time > now || now - report->host_time > REPORT_MAXIMUM_AGE_TICKS)
		return FALSE;
	ticks = MIN(now - report->host_time + TARGET_HISTORY_SLACK_TICKS, TARGET_HISTORY_TICKS - 1);
	/* damage that player could deal, and how they deal it */
	rate = distributed_player_deals(player_index, report->damage.definition_index, &kinds, &fired_reach);
	if (rate <= 0.0f)
		return FALSE;
	/* ... of its shape: at a point, else a melee blow, else a collision; and
	area damage as the game deals it (the report's only when the damage is
	dealt both ways) */
	if ((TEST_FLAG(kinds, _damage_source_impact_bit) || TEST_FLAG(kinds, _damage_source_area_bit)) &&
		distributed_distance_squared(&report->damage.origin, &report->damage.epicenter) <=
			REPORT_POINT_TOLERANCE * REPORT_POINT_TOLERANCE)
	{
		if (!TEST_FLAG(kinds, _damage_source_impact_bit))
			area = TRUE;
		else if (!TEST_FLAG(kinds, _damage_source_area_bit))
			area = FALSE;
		else
			area = TEST_FLAG(report->damage.flags, _damage_area_of_effect_bit);
	}
	else if (TEST_FLAG(kinds, _damage_source_melee_bit))
	{
		melee = TRUE;
		area = TRUE;
	}
	else if (TEST_FLAG(kinds, _damage_source_collision_bit))
	{
		collision = TRUE;
		area = TRUE;
	}
	else
	{
		return FALSE;
	}
	/* ... no harder than it can be (all of it, but an airborne melee
	blow's) */
	if (!(report->damage.scale >= 0.0f && report->damage.scale <= (melee ? REPORT_MAXIMUM_MELEE_SCALE : 1.0f)))
		return FALSE;
	/* the origin at the target (an explosion's within its reach) */
	definition = damage_effect_definition_get(report->damage.definition_index);
	reach = target->object.bounding_sphere_radius + REPORT_IMPACT_TOLERANCE;
	if (area)
		reach += definition->cutoff_radius;
	if (!(distributed_distance_squared(&report->damage.origin, &report->target_position) <= reach * reach))
		return FALSE;
	/* no more than the weapon fires */
	if (!distributed_report_paid(player_index, report, rate, area && !melee && !collision,
		definition->cutoff_radius > 0.0f))
	{
		return FALSE;
	}
	/* a melee blow from where the host had the player (its origin their
	head, its epicenter their body); a collision from the vehicle they
	drove */
	if (melee && (!distributed_player_was_near(player_index, &report->damage.epicenter, ticks) ||
		!distributed_player_was_near(player_index, &report->damage.origin, ticks)))
	{
		return FALSE;
	}
	if (collision && !distributed_player_drove_into(player_index, &report->damage.epicenter, &report->target_position,
		target->object.bounding_sphere_radius, ticks))
	{
		return FALSE;
	}
	/* what hits at a point (a shot, an explosion) within its reach of where
	the host had the player since it was fired, where the tags bound that
	(no line of sight: a wall between is not looked for) */
	if (!melee && !collision && fired_reach.distance >= 0.0f &&
		!distributed_player_could_fire(machine_index, player_index, &report->damage.origin, &fired_reach,
			report->host_time))
	{
		return FALSE;
	}
	/* the target about where the host had it when the shooter saw it: a
	player's unit or vehicle as far back as the report was made, else (what
	no player has) about where it is */
	seen = distributed_target_seen(report->object_index, &report->target_position, ticks);
	if (seen == FALSE)
		return FALSE;
	if (seen == NONE)
	{
		real_point3d origin;
		real_vector3d velocity;
		real tolerance;

		object_get_origin(report->object_index, &origin);
		/* (a rider's own velocity is not kept: what it rides has it) */
		object_get_velocities(report->object_index, &velocity, NULL);
		tolerance = REPORT_TARGET_TOLERANCE + REPORT_TARGET_LEAD_TICKS *
			(real)sqrt(velocity.i * velocity.i + velocity.j * velocity.j + velocity.k * velocity.k);
		if (!(distributed_distance_squared(&report->target_position, &origin) <= tolerance * tolerance))
			return FALSE;
	}
	/* the damage as the host deals it: what a client's hit can be, from the
	player's own */
	if (!distributed_damage_to_data(&report->damage, damage))
		return FALSE;
	damage->flags &= REPORT_DAMAGE_FLAGS;
	SET_FLAG(damage->flags, _damage_area_of_effect_bit, area);
	/* ... an explosion's direction and scale as the game gives them
	(damage.c's area_of_effect_cause_damage_to_object): from the epicenter
	to the target's centre where the shooter saw it, and the less of the
	report's and its fall off over that, unless it does not fall off; any
	other's direction one long */
	if (area && !melee && !collision)
	{
		real_point3d origin;
		real distance;

		object_get_origin(report->object_index, &origin);
		damage->direction.i = report->target_position.x + target->object.bounding_sphere_center.x - origin.x -
			damage->epicenter.x;
		damage->direction.j = report->target_position.y + target->object.bounding_sphere_center.y - origin.y -
			damage->epicenter.y;
		damage->direction.k = report->target_position.z + target->object.bounding_sphere_center.z - origin.z -
			damage->epicenter.z;
		distance = normalize3d(&damage->direction);
		if (!TEST_FLAG(definition->flags, _damage_effect_dont_scale_damage_by_distance_bit))
		{
			real radius_delta = definition->cutoff_radius - definition->falloff_radius;
			real scale = radius_delta > 0.0f ?
				PIN(1.0f - (distance - definition->falloff_radius) / radius_delta, 0.0f, 1.0f) : 1.0f;

			damage->scale = MIN(damage->scale, scale);
		}
	}
	else
	{
		normalize3d(&damage->direction);
	}
	damage->multiplier = 1.0f;
	damage->owner_player_index = DATUM_INDEX_NEW(player_index, player->identifier);
	damage->owner_team_index = (short)player->team_index;
	/* (the players' traits' multipliers go by the object) */
	unit_index = distributed_living_unit(player);
	if (!distributed_object_is_players(player_index, unit_index, damage->owner_object_index))
		damage->owner_object_index = unit_index;
	return TRUE;
}

void network_damage_handle_reports(
	long machine_index,
	void const *entries,
	short count)
{
	struct distributed_hit_report const *reports = (struct distributed_hit_report const *)entries;
	short index;

	for (index = 0; index < count; index++)
	{
		struct distributed_hit_report const *report = &reports[index];
		struct damage_data damage;

		if (!distributed_report_valid(machine_index, report, &damage))
		{
			damage_rejected_reports++;
			continue;
		}
		damage_dealt_reports++;
		damage_dealing_report = TRUE;
		object_cause_damage(&damage, report->object_index, report->node_index, report->region_index,
			report->material_index, report->has_normal ? &report->object_normal : NULL);
		damage_dealing_report = FALSE;
	}
}

/* where each damage event of this tick goes: a player effect to its
player's machine; damage to a unit to the machines of the players it
concerns (the unit's and its riders', as the history has them this tick)
and of those sent them this tick (who see them), and its owner's; a killing
blow to all (NONE) */
static void distributed_note_event_destinations(
	void)
{
	struct damage_history_tick const *tick = &damage_history[game_time_get() & (TARGET_HISTORY_TICKS - 1)];
	short index;

	for (index = 0; index < damage_event_count; index++)
	{
		struct distributed_damage_event const *event = &damage_events[index];
		struct unit_datum *unit;
		short *count = &damage_event_destinations[index].player_count;
		short *player_indices = damage_event_destinations[index].player_indices;
		short player_index;

		damage_event_destinations[index].owner_machine_index = NONE;
		*count = 0;
		if (event->kind == _damage_event_player_effect)
		{
			player_indices[(*count)++] = event->player_index;
			continue;
		}
		if (event->kind != _damage_event_aftermath)
		{
			*count = NONE;
			continue;
		}
		if (event->damage.owner_player_index != NO_PLAYER)
		{
			damage_event_destinations[index].owner_machine_index =
				distributed_player_machine(event->damage.owner_player_index);
		}
		/* (a player's body, living or dead) */
		unit = (struct unit_datum *)object_try_and_get_and_verify_type(event->object_index, _object_mask_unit);
		if (unit)
		{
			long unit_player_index = unit->unit.player_index;

			/* (a body's player is no longer its unit's, but has it as their
			dead one, player_died) */
			if (unit_player_index == NONE)
			{
				struct data_iterator iterator;
				struct player_datum *player;

				data_iterator_new(&iterator, player_data);
				while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
				{
					if (player->dead_unit_index == event->object_index)
					{
						unit_player_index = iterator.datum_index;
						break;
					}
				}
			}
			if (unit_player_index != NONE)
				player_indices[(*count)++] = (short)DATUM_INDEX_TO_ABSOLUTE_INDEX(unit_player_index);
		}
		if (tick->time != game_time_get())
			continue;
		for (player_index = 0; player_index < MAXIMUM_TRACKED_PLAYERS; player_index++)
		{
			if ((tick->objects[player_index][0].object_index == event->object_index ||
				tick->objects[player_index][1].object_index == event->object_index) &&
				(!*count || player_indices[0] != player_index))
			{
				if (*count >= MAXIMUM_EVENT_PLAYERS)
				{
					*count = NONE;
					break;
				}
				player_indices[(*count)++] = player_index;
			}
		}
	}
}

/* whether the damage event goes to the machine */
static boolean distributed_event_goes_to(
	short event_index,
	long machine_index)
{
	short count = damage_event_destinations[event_index].player_count;
	short index;

	if (count == NONE || damage_event_destinations[event_index].owner_machine_index == machine_index)
		return TRUE;
	for (index = 0; index < count; index++)
	{
		short player_index = damage_event_destinations[event_index].player_indices[index];

		if (distributed_player_machine(player_index) == machine_index ||
			(damage_events[event_index].kind == _damage_event_aftermath &&
				distributed_machine_sees_player(machine_index, player_index)))
		{
			return TRUE;
		}
	}
	return FALSE;
}

void network_damage_host_tick(
	void)
{
	struct distributed_damage_event_message message;
	short limit = MIN(MAXIMUM_ENTRIES_PER_MESSAGE, RELIABLE_ENTRIES(struct distributed_damage_event));
	long machine_indices[HALO_PORT_MAXIMUM_NETWORK_MACHINES];
	short machine_count = damage_event_count ?
		distributed_client_machines(machine_indices, HALO_PORT_MAXIMUM_NETWORK_MACHINES) : 0;
	short machine_number;

	distributed_note_weapons();
	distributed_note_targets();
	distributed_note_event_destinations();
	for (machine_number = 0; machine_number < machine_count; machine_number++)
	{
		long machine_index = machine_indices[machine_number];
		short index;
		short count = 0;

		for (index = 0; index < damage_event_count; index++)
		{
			if (!distributed_event_goes_to(index, machine_index))
				continue;
			message.events[count++] = damage_events[index];
			if (count == limit)
			{
				distributed_send_to_machine(machine_index, &message, _distributed_message_damage_events, count,
					(word)(sizeof(message.header) + count * sizeof(struct distributed_damage_event)));
				count = 0;
			}
		}
		if (count)
		{
			distributed_send_to_machine(machine_index, &message, _distributed_message_damage_events, count,
				(word)(sizeof(message.header) + count * sizeof(struct distributed_damage_event)));
		}
	}
	damage_event_count = 0;
}

/* ---------- a client */

void network_damage_handle_events(
	void const *entries,
	short count)
{
	struct distributed_damage_event const *events = (struct distributed_damage_event const *)entries;
	short index;

	for (index = 0; index < count; index++)
	{
		struct distributed_damage_event const *event = &events[index];
		struct damage_data damage;

		if (!distributed_damage_numbers_valid(&event->damage) || !distributed_damage_to_data(&event->damage, &damage) ||
			!distributed_real_valid(event->shield_damage) || !distributed_real_valid(event->body_damage) ||
			!distributed_real_valid(event->body_damage_multiplier) || !distributed_real_valid(event->total_damage))
		{
			continue;
		}
		damage_replayed_events++;
		switch (event->kind)
		{
		case _damage_event_player_effect:
		{
			long player_index = distributed_player_from_byte(event->player_index);

			if (distributed_player_is_local(player_index))
				damage_replay_player_effect(player_index, &damage, event->total_damage);
			break;
		}
		case _damage_event_aftermath:
			if (network_objects_client_has(event->object_index) &&
				object_try_and_get_and_verify_type(event->object_index, _object_mask_unit))
			{
				/* (what harm it did the units' states bring: no death from here) */
				damage_replay_aftermath(event->object_index, &damage,
					event->being_damaged_flags &
						~(FLAG(_object_being_damaged_body_depleted_bit) | FLAG(_object_being_damaged_killed_instantly_bit)),
					event->shield_damage, event->body_damage, event->body_damage_multiplier, event->body_part);
			}
			break;
		case _damage_event_kill:
			/* (a biped's: a vehicle's riders the blow would kill too) */
			if (network_objects_client_has(event->object_index) &&
				object_try_and_get_and_verify_type(event->object_index, _object_mask_biped) &&
				!TEST_FLAG(object_get(event->object_index)->object.damage_flags, _object_dead_bit) &&
				distributed_damage_indices_valid(event->object_index, event->node_index, event->region_index,
					event->material_index))
			{
				struct unit_datum *unit = unit_get(event->object_index);
				/* (the blow's aftermath takes the unit's player from it) */
				long victim_player_index = unit->unit.player_index;

				if (unit->unit.player_index != NONE)
				{
					distributed_set_death((short)DATUM_INDEX_TO_ABSOLUTE_INDEX(unit->unit.player_index),
						event->player_index, TEST_FLAG(event->kill_flags, _damage_event_friendly_fire_bit),
						TEST_FLAG(event->kill_flags, _damage_event_killed_by_vehicle_bit));
				}
				/* (no player's, an actor's: nothing of it counted here, the
				host's statistics come as they are) */
				else
				{
					SET_FLAG(damage.flags, _damage_no_statistics_bit, TRUE);
				}
				damage_replaying_kill = TRUE;
				damage_replaying_killer = event->player_index != NO_PLAYER ?
					distributed_player_from_byte(event->player_index) : NONE;
				damage_replaying_killer_score = event->killer_score;
				damage_replay_kill(event->object_index, &damage, event->node_index, event->region_index,
					event->material_index);
				damage_replaying_kill = FALSE;
				damage_replaying_killer = NONE;
				if (victim_player_index != NONE && TEST_FLAG(event->kill_flags, _damage_event_telefragged_bit))
					players_show_telefragged(victim_player_index);
			}
			break;
		}
	}
}

void network_damage_client_tick(
	void)
{
	struct distributed_hit_report_message message;
	short limit = MIN(MAXIMUM_ENTRIES_PER_MESSAGE, RELIABLE_ENTRIES(struct distributed_hit_report));
	short index;
	short count = 0;

	damage_sent_reports += damage_report_count;
	for (index = 0; index < damage_report_count; index++)
	{
		message.reports[count++] = damage_reports[index];
		if (count == limit)
		{
			distributed_send(&message, _distributed_message_hit_reports, count,
				(word)(sizeof(message.header) + count * sizeof(struct distributed_hit_report)),
				_distributed_to_host_reliably);
			count = 0;
		}
	}
	if (count)
	{
		distributed_send(&message, _distributed_message_hit_reports, count,
			(word)(sizeof(message.header) + count * sizeof(struct distributed_hit_report)),
			_distributed_to_host_reliably);
	}
	damage_report_count = 0;
}

void network_damage_statistics(
	long *sent_reports,
	long *dealt_reports,
	long *rejected_reports,
	long *replayed_events)
{
	*sent_reports = damage_sent_reports;
	*dealt_reports = damage_dealt_reports;
	*rejected_reports = damage_rejected_reports;
	*replayed_events = damage_replayed_events;
}

void network_damage_new_game(
	void)
{
	short player_index;

	damage_event_count = 0;
	damage_report_count = 0;
	damage_dealing_report = FALSE;
	damage_replaying_kill = FALSE;
	csmemset(damage_players, 0, sizeof(damage_players));
	for (player_index = 0; player_index < TARGET_HISTORY_TICKS; player_index++)
		damage_history[player_index].time = NONE;
	for (player_index = 0; player_index < MAXIMUM_TRACKED_PLAYERS; player_index++)
	{
		short index;

		for (index = 0; index < MAXIMUM_RECENT_WEAPONS; index++)
			damage_players[player_index].definition_indices[index] = NONE;
		damage_players[player_index].unarmed_melee_damage_index = NONE;
		for (index = 0; index < NUMBER_OF_UNIT_GRENADE_TYPES; index++)
			damage_players[player_index].grenade_times[index] = -RECENT_WEAPON_TICKS - 1;
		damage_players[player_index].driven_time = -RECENT_WEAPON_TICKS - 1;
		damage_players[player_index].hit_seconds = HIT_REPORT_BURST_SECONDS;
		damage_explosions[player_index].time = NONE;
		damage_explosions[player_index].count = 0;
		for (index = 0; index < PLAYER_TRAIL_POINTS; index++)
			damage_trails[player_index][index].time = NONE;
	}
	for (player_index = 0; player_index < DAMAGE_RATE_CACHE_SIZE; player_index++)
		damage_rate_cache[player_index].source_index = NONE;
}
