/*
PAL_TAGS.C

The European (PAL) release's maps, built by 01.01.14.2342, are made for a PAL
Xbox, which ran the game's 30 ticks a second at 25 real ones: a few of their
tags carry the North American (NTSC) release's values times 30/25 (speeds,
rates of fire) or its square (the Warthog's gravity), so that those things
went as fast in real time as on an NTSC Xbox. The native builds run 30 real
ticks a second on either release's maps, which made the PAL maps' players
run a fifth faster and jump higher, their plasma rifles and needlers fire a
fifth faster, and so on; and in a game of both, each machine predicts its own
players from its own maps (network_distributed.c), which the host then
corrected.

So when the tags of a PAL map load (scenario_tags_load), each such value is
put back to the NTSC maps' (01.10.12.2276, the same in all three NTSC
releases): the same values in every map, multiplayer and campaign, found by
comparing the two releases' maps tag by tag. A PAL map is one of any build
cache_files.c lists as PAL (cache_files_build_region), where other PAL
releases' builds go too. A value is set, and a first-person animation paced,
only where it has exactly the 01.01.14.2342 maps' value or frame count, so
that a map built otherwise stays as it is.

The PAL maps' first-person weapon animations have fewer frames too
(resampled for 25 ticks a second), and a weapon's readying, reloading and
its player's melee last as many ticks as its first-person animation has
frames (weapons.c, weapon_get_first_person_animation_time; bipeds.c). Their
frame data cannot take more frames, so a PAL map's weapons take their times
from the NTSC maps' frame counts (pal_tags_first_person_frames), and their
first-person animations play at the pace that ends them on time
(pal_tags_first_person_advance, first_person_weapons.c), drawn between their
frames (pal_tags_first_person_fraction) so that they move every tick.

The releases' maps also differ in the multiplayer strings (text_group.c has
the ones the NTSC maps lack), some tags of campaign menus and the layout of
the files: nothing played or sent between machines.
*/

#include "cseries.h"
#include "cache/cache_files.h"
#include "game/game_globals.h"
#include "game/players.h"
#include "items/projectile_definitions.h"
#include "items/weapon_definitions.h"
#include "models/model_animation_definitions.h"
#include "physics/physics_definitions.h"
#include "tag_files/tag_groups.h"
#include "units/biped_definitions.h"
#include "units/vehicle_definitions.h"

#include <string.h>

/* the platform layer's (port/linux/src) */
void platform_log(char const *format, ...);

/* ---------- constants */

/* the sizes and fields of records that their own units keep to themselves
(bipeds.c's struct game_globals_falling_damage, vehicles.c's struct
vehicle_definition) */
enum
{
	FALLING_DISTANCE_LOWER_BOUND_OFFSET = 0x8,
	FALLING_DISTANCE_UPPER_BOUND_OFFSET = 0xC,
	FALLING_MAXIMUM_DISTANCE_OFFSET = 0x28,
	FALLING_RUNTIME_MAXIMUM_FALLING_VELOCITY_OFFSET = 0x8C,
	FALLING_RUNTIME_MINIMUM_DAMAGE_VELOCITY_OFFSET = 0x90,
	FALLING_RUNTIME_MAXIMUM_DAMAGE_VELOCITY_OFFSET = 0x94,

	/* (the vehicle's maximum forward speed, speed acceleration, and maximum
	left and right turn) */
	VEHICLE_MAXIMUM_FORWARD_SPEED_OFFSET = 0x2F8,
	VEHICLE_SPEED_ACCELERATION_OFFSET = 0x300,
	VEHICLE_MAXIMUM_LEFT_TURN_OFFSET = 0x308,
	VEHICLE_MAXIMUM_RIGHT_TURN_OFFSET = 0x30C,
};

/* the first-person animations whose frame counts differ, each graph's
animation (by index, the same in both releases' graphs) with its PAL and
NTSC frame counts: the same in every map */
static struct
{
	char const *graph;
	short animation_index;
	short pal_frames;
	short ntsc_frames;
} const pal_first_person_animations[] =
{
	{ "weapons\\assault rifle\\fp\\fp", 3, 30, 37 },	/* melee */
	{ "weapons\\assault rifle\\fp\\fp", 8, 23, 29 },	/* ready */
	{ "weapons\\assault rifle\\fp\\fp", 9, 72, 87 },	/* reload-full */
	{ "weapons\\assault rifle\\fp\\fp", 10, 72, 87 },	/* reload-full2 */
	{ "weapons\\ball\\fp\\fp", 1, 23, 28 },	/* melee */
	{ "weapons\\ball\\fp\\fp", 4, 68, 83 },	/* posing */
	{ "weapons\\ball\\fp\\fp", 5, 23, 28 },	/* ready */
	{ "weapons\\flag\\fp\\fp", 1, 26, 40 },	/* melee */
	{ "weapons\\flag\\fp\\fp", 4, 62, 75 },	/* posing */
	{ "weapons\\flag\\fp\\fp", 5, 17, 21 },	/* ready */
	{ "weapons\\needler\\fp\\fp", 3, 39, 48 },	/* melee */
	{ "weapons\\needler\\fp\\fp", 6, 73, 88 },	/* posing */
	{ "weapons\\needler\\fp\\fp", 7, 18, 23 },	/* ready */
	{ "weapons\\needler\\fp\\fp", 8, 58, 70 },	/* reload-full */
	{ "weapons\\pistol\\fp\\fp", 2, 37, 45 },	/* melee */
	{ "weapons\\pistol\\fp\\fp", 5, 110, 133 },	/* posing */
	{ "weapons\\pistol\\fp\\fp", 6, 28, 35 },	/* ready */
	{ "weapons\\pistol\\fp\\fp", 7, 55, 67 },	/* reload-empty */
	{ "weapons\\pistol\\fp\\fp", 8, 53, 65 },	/* reload-full */
	{ "weapons\\plasma pistol\\fp\\fp", 3, 27, 33 },	/* melee */
	{ "weapons\\plasma pistol\\fp\\fp", 6, 19, 24 },	/* o-h-exit */
	{ "weapons\\plasma pistol\\fp\\fp", 7, 19, 24 },	/* o-h-s-enter */
	{ "weapons\\plasma pistol\\fp\\fp", 14, 98, 119 },	/* posing */
	{ "weapons\\plasma pistol\\fp\\fp", 15, 11, 14 },	/* ready */
	{ "weapons\\plasma rifle\\fp\\fp", 2, 29, 36 },	/* melee */
	{ "weapons\\plasma rifle\\fp\\fp", 5, 28, 35 },	/* o-h-exit */
	{ "weapons\\plasma rifle\\fp\\fp", 7, 48, 58 },	/* overheating */
	{ "weapons\\plasma rifle\\fp\\fp", 10, 87, 105 },	/* posing */
	{ "weapons\\plasma rifle\\fp\\fp", 11, 23, 29 },	/* ready */
	{ "weapons\\rocket launcher\\fp\\fp", 0, 23, 28 },	/* fire-1 */
	{ "weapons\\rocket launcher\\fp\\fp", 2, 43, 52 },	/* melee */
	{ "weapons\\rocket launcher\\fp\\fp", 5, 98, 118 },	/* posing */
	{ "weapons\\rocket launcher\\fp\\fp", 6, 18, 22 },	/* ready */
	{ "weapons\\rocket launcher\\fp\\fp", 7, 103, 125 },	/* reload-empty */
	{ "weapons\\rocket launcher\\fp\\fp", 8, 92, 111 },	/* reload-full */
	{ "weapons\\shotgun\\fp\\fp", 0, 12, 15 },	/* enter */
	{ "weapons\\shotgun\\fp\\fp", 1, 34, 42 },	/* exit-empty */
	{ "weapons\\shotgun\\fp\\fp", 2, 19, 24 },	/* exit-full */
	{ "weapons\\shotgun\\fp\\fp", 3, 21, 26 },	/* fire-1 */
	{ "weapons\\shotgun\\fp\\fp", 5, 29, 36 },	/* melee */
	{ "weapons\\shotgun\\fp\\fp", 8, 54, 66 },	/* posing */
	{ "weapons\\shotgun\\fp\\fp", 9, 18, 23 },	/* ready */
	{ "weapons\\shotgun\\fp\\fp", 10, 9, 12 },	/* reload-empty */
	{ "weapons\\sniper rifle\\fp\\fp", 2, 29, 36 },	/* melee */
	{ "weapons\\sniper rifle\\fp\\fp", 5, 138, 166 },	/* posing */
	{ "weapons\\sniper rifle\\fp\\fp", 6, 23, 29 },	/* ready */
	{ "weapons\\sniper rifle\\fp\\fp", 7, 78, 94 },	/* reload-empty */
	{ "weapons\\sniper rifle\\fp\\fp", 8, 68, 83 },	/* reload-full */
};

/* ---------- globals */

static struct
{
	short restored;
	short checked;
	/* the loaded map's graphs (tag indices) of pal_first_person_animations,
	NONE where the map has not the graph or not the PAL frame count */
	long first_person_graphs[NUMBEROF(pal_first_person_animations)];
	/* each local player's first-person animation and how far through its
	next frame it is (pal_tags_first_person_advance) */
	struct
	{
		long graph_index;
		short animation_index;
		short frame_index;
		real fraction;
	} advancing[MAXIMUM_LOCAL_PLAYERS];
} pal_tags;

/* ---------- private code */

/* the NTSC maps' value (as bits) for a word holding exactly the PAL maps' */
static void pal_tags_restore(
	void *field,
	unsigned long pal_bits,
	unsigned long ntsc_bits)
{
	unsigned long bits;

	pal_tags.checked++;
	csmemcpy(&bits, field, sizeof(bits));
	if (bits == pal_bits)
	{
		csmemcpy(field, &ntsc_bits, sizeof(ntsc_bits));
		pal_tags.restored++;
	}
}

static void *pal_tags_get(
	unsigned long group_tag,
	char const *name)
{
	long index = tag_loaded(group_tag, name);

	return index == NONE ? NULL : tag_get(group_tag, index);
}

/* a block's first element, if it has one */
static byte *pal_tags_first_element(
	struct tag_block const *block)
{
	return block->count > 0 && block->address ? (byte *)xbox_pointer(block->address) : NULL;
}

static void pal_tags_restore_bipeds(
	void)
{
	static char const *const cyborgs[] =
	{
		"characters\\cyborg\\cyborg",
		"characters\\cyborg_mp\\cyborg_mp",
	};
	struct biped_definition *definition;
	short index;

	for (index = 0; index < NUMBEROF(cyborgs); index++)
	{
		definition = pal_tags_get(BIPED_DEFINITION_TAG, cyborgs[index]);
		if (!definition)
			continue;
		/* 5.894 (x1.44), 0.07 world units a tick (x1.2) */
		pal_tags_restore(&definition->biped.moving_turning_speed, 0x4107C5CD, 0x40BC9B76);
		pal_tags_restore(&definition->biped.jump_velocity, 0x3DAC0831, 0x3D8F5C29);
	}
	/* (the campaign's cutscene Master Chief: the PAL maps' has the old
	player physics, bipeds.c's fixed NTSC speeds) */
	definition = pal_tags_get(BIPED_DEFINITION_TAG, "characters\\cyborg\\cyborg_cinematic");
	if (definition)
		pal_tags_restore(&definition->biped.flags, 0x00001002, 0x00000002);
}

static void pal_tags_restore_globals(
	void)
{
	struct game_globals *globals = pal_tags_get(GAME_GLOBALS_TAG, "globals\\globals");
	struct game_globals_player_information *player_information;
	byte *falling_damage;

	if (!globals)
		return;
	player_information = (struct game_globals_player_information *)pal_tags_first_element(&globals->player_information);
	if (player_information)
	{
		/* 0.512, 2.25, 2 and 2 world units a second (x1.2) */
		pal_tags_restore(&player_information->walking_speed, 0x3F1D2F1B, 0x3F03126F);
		pal_tags_restore(&player_information->run_forward_speed, 0x402CCCCD, 0x40100000);
		pal_tags_restore(&player_information->run_backward_speed, 0x4019999A, 0x40000000);
		pal_tags_restore(&player_information->run_sideways_speed, 0x4019999A, 0x40000000);
	}
	falling_damage = pal_tags_first_element(&globals->falling_damage);
	if (falling_damage)
	{
		/* the distances a fall starts and stops hurting at, and kills at (3,
		6, 16.5: the PAL maps' are higher, for their higher jumps), and the
		velocities worked out from them */
		pal_tags_restore(falling_damage + FALLING_DISTANCE_LOWER_BOUND_OFFSET, 0x4095C28F, 0x40400000);
		pal_tags_restore(falling_damage + FALLING_DISTANCE_UPPER_BOUND_OFFSET, 0x4115C28F, 0x40C00000);
		pal_tags_restore(falling_damage + FALLING_MAXIMUM_DISTANCE_OFFSET, 0x41C00000, 0x41840000);
		pal_tags_restore(falling_damage + FALLING_RUNTIME_MAXIMUM_FALLING_VELOCITY_OFFSET, 0x3ED3CD76, 0x3EAF9E10);
		pal_tags_restore(falling_damage + FALLING_RUNTIME_MINIMUM_DAMAGE_VELOCITY_OFFSET, 0x3E3B0F19, 0x3E15C45D);
		pal_tags_restore(falling_damage + FALLING_RUNTIME_MAXIMUM_DAMAGE_VELOCITY_OFFSET, 0x3E84454B, 0x3E53CD76);
	}
}

static void pal_tags_restore_weapons(
	void)
{
	static struct
	{
		char const *name;
		unsigned long pal_initial, ntsc_initial;
		unsigned long pal_final, ntsc_final;
	} const weapons[] =
	{
		/* rounds a second: 7 to 10, 3 to 10 (x1.2) */
		{ "weapons\\plasma rifle\\plasma rifle", 0x41066666, 0x40E00000, 0x41400000, 0x41200000 },
		{ "weapons\\needler\\needler", 0x40666666, 0x40400000, 0x41400000, 0x41200000 },
	};
	short index;

	for (index = 0; index < NUMBEROF(weapons); index++)
	{
		struct weapon_definition *definition = pal_tags_get(WEAPON_DEFINITION_TAG, weapons[index].name);
		struct weapon_trigger_definition *trigger;

		if (!definition)
			continue;
		trigger = (struct weapon_trigger_definition *)pal_tags_first_element(&definition->weapon.triggers);
		if (!trigger)
			continue;
		pal_tags_restore(&trigger->initial_rate_of_fire, weapons[index].pal_initial, weapons[index].ntsc_initial);
		pal_tags_restore(&trigger->final_rate_of_fire, weapons[index].pal_final, weapons[index].ntsc_final);
	}
}

static void pal_tags_restore_projectiles(
	void)
{
	struct projectile_definition *definition = pal_tags_get(PROJECTILE_DEFINITION_TAG, "weapons\\needler\\needle");

	if (!definition)
		return;
	/* 0.1333 world units a tick (x1.2) */
	pal_tags_restore(&definition->projectile.initial_velocity, 0x3E23D70B, 0x3E088889);
	pal_tags_restore(&definition->projectile.final_velocity, 0x3E23D70B, 0x3E088889);
}

static void pal_tags_find_first_person_animations(
	void)
{
	short index;

	for (index = 0; index < NUMBEROF(pal_first_person_animations); index++)
	{
		long graph_index = tag_loaded(ANIMATION_GRAPH_TAG, pal_first_person_animations[index].graph);
		struct animation_graph *graph;

		pal_tags.first_person_graphs[index] = NONE;
		if (graph_index == NONE)
			continue;
		graph = animation_graph_definition_get(graph_index);
		pal_tags.checked++;
		if (pal_first_person_animations[index].animation_index < graph->animations.count &&
			TAG_BLOCK_GET_ELEMENT(&graph->animations, pal_first_person_animations[index].animation_index,
				struct animation)->frame_count == pal_first_person_animations[index].pal_frames)
		{
			pal_tags.first_person_graphs[index] = graph_index;
			pal_tags.restored++;
		}
	}
}

static void pal_tags_restore_warthog(
	void)
{
	byte *vehicle = pal_tags_get(VEHICLE_DEFINITION_TAG, "vehicles\\warthog\\warthog");
	struct physics_definition *physics = pal_tags_get(POINT_PHYSICS_DEFINITION_TAG, "vehicles\\warthog\\warthog");

	if (vehicle)
	{
		/* 0.255 world units a tick, 0.00275, 30 and -30 degrees (x1.2) */
		pal_tags_restore(vehicle + VEHICLE_MAXIMUM_FORWARD_SPEED_OFFSET, 0x3E9CAC08, 0x3E828F5C);
		pal_tags_restore(vehicle + VEHICLE_SPEED_ACCELERATION_OFFSET, 0x3B5844D0, 0x3B343958);
		pal_tags_restore(vehicle + VEHICLE_MAXIMUM_LEFT_TURN_OFFSET, 0x42100000, 0x41F00000);
		pal_tags_restore(vehicle + VEHICLE_MAXIMUM_RIGHT_TURN_OFFSET, 0xC2100000, 0xC1F00000);
	}
	if (physics)
	{
		/* 1 (x1.44) */
		pal_tags_restore(&physics->gravity_scale, 0x3FB851EC, 0x3F800000);
	}
}

/* the entry of pal_first_person_animations for an animation of the loaded
map's graph, if it is one (a PAL map's) */
static short pal_tags_first_person_entry(
	long graph_index,
	short animation_index)
{
	short index;

	for (index = 0; index < NUMBEROF(pal_first_person_animations); index++)
	{
		if (pal_tags.first_person_graphs[index] == graph_index &&
			pal_first_person_animations[index].animation_index == animation_index)
		{
			return index;
		}
	}
	return NONE;
}

/* ---------- public code */

/* the tags of the map just loaded, built by build: a PAL map's (one of a
build the cache files list as PAL, cache_files_build_region) as the NTSC
maps' (cache_files.c, scenario_tags_load) */
void pal_tags_loaded(
	char const *build)
{
	char const *region;
	short index;

	csmemset(&pal_tags, 0, sizeof(pal_tags));
	for (index = 0; index < NUMBEROF(pal_tags.first_person_graphs); index++)
		pal_tags.first_person_graphs[index] = NONE;
	for (index = 0; index < MAXIMUM_LOCAL_PLAYERS; index++)
		pal_tags.advancing[index].graph_index = NONE;
	region = cache_files_build_region(build);
	if (!region || strcmp(region, "PAL"))
		return;
	pal_tags_find_first_person_animations();
	pal_tags_restore_bipeds();
	pal_tags_restore_globals();
	pal_tags_restore_weapons();
	pal_tags_restore_projectiles();
	pal_tags_restore_warthog();
	platform_log("PAL map (%.32s): %d of %d values and first-person animations as the NTSC maps'", build, pal_tags.restored, pal_tags.checked);
}

/* the frames a first-person animation lasts for the game's timing: a PAL
map's the NTSC maps' (weapons.c, weapon_get_first_person_animation_time) */
short pal_tags_first_person_frames(
	long graph_index,
	short animation_index,
	short frames)
{
	short index = graph_index == NONE ? NONE : pal_tags_first_person_entry(graph_index, animation_index);

	return index == NONE ? frames : pal_first_person_animations[index].ntsc_frames;
}

/* whether a local player's first-person animation advances a frame this
tick: a PAL map's with fewer frames than the NTSC maps' stays on a frame now
and then, so that it lasts as many ticks as the NTSC maps' (and as the
game's timing, pal_tags_first_person_frames) (first_person_weapons.c); it is
drawn between that frame and the next (pal_tags_first_person_fraction), so
that it moves every tick rather than stopping on the frame it stays on */
boolean pal_tags_first_person_advance(
	short local_player_index,
	long graph_index,
	short animation_index,
	short frame_index)
{
	short index;

	if (local_player_index < 0 || local_player_index >= MAXIMUM_LOCAL_PLAYERS)
		return TRUE;
	/* (another animation, or this one again from its start; not a state
	holding its last frame, first_person_weapon_next_state) */
	if (pal_tags.advancing[local_player_index].graph_index != graph_index ||
		pal_tags.advancing[local_player_index].animation_index != animation_index ||
		(frame_index == 0 && pal_tags.advancing[local_player_index].frame_index > 0))
	{
		pal_tags.advancing[local_player_index].graph_index = graph_index;
		pal_tags.advancing[local_player_index].animation_index = animation_index;
		pal_tags.advancing[local_player_index].fraction = 0.0f;
	}
	pal_tags.advancing[local_player_index].frame_index = frame_index;
	index = pal_tags_first_person_entry(graph_index, animation_index);
	if (index == NONE)
		return TRUE;
	pal_tags.advancing[local_player_index].fraction +=
		(real)pal_first_person_animations[index].pal_frames / pal_first_person_animations[index].ntsc_frames;
	if (pal_tags.advancing[local_player_index].fraction < 1.0f)
		return FALSE;
	pal_tags.advancing[local_player_index].fraction -= 1.0f;
	return TRUE;
}

/* how far a local player's first-person animation, at frame_index, is on
its way to the next frame (0 up to 1), for drawing it between the two: a
PAL map's that pal_tags_first_person_advance slows, 0 for any other (or for
this one just started over) (first_person_weapons.c) */
real pal_tags_first_person_fraction(
	short local_player_index,
	long graph_index,
	short animation_index,
	short frame_index)
{
	if (local_player_index < 0 || local_player_index >= MAXIMUM_LOCAL_PLAYERS ||
		pal_tags.advancing[local_player_index].graph_index != graph_index ||
		pal_tags.advancing[local_player_index].animation_index != animation_index ||
		frame_index < pal_tags.advancing[local_player_index].frame_index ||
		pal_tags_first_person_entry(graph_index, animation_index) == NONE)
	{
		return 0.0f;
	}
	return pal_tags.advancing[local_player_index].fraction;
}
