"""Execute the production typed object checkpoint functions with bounded APIs.

The extracted function bodies are compiled verbatim. Only the surrounding object
registry/physics APIs are replaced, so generation checks, ammunition preservation,
projectile replay suppression and snapshot validation run as implemented.
"""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def declaration(text, marker):
    start = text.index(marker)
    opening = text.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    if text[end:end + 1] == ';':
        end += 1
    return text[start:end]


class CheckpointRuntime(unittest.TestCase):
    def compile_and_run(self, code, expected):
        with tempfile.TemporaryDirectory(prefix='halo-checkpoint-runtime-') as tmp:
            c = Path(tmp) / 'checkpoint.c'
            binary = Path(tmp) / 'checkpoint'
            c.write_text(code)
            compiler = os.environ.get('CC', 'cc')
            build = subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra',
                                    '-fsanitize=address,undefined', '-g', str(c), '-o', str(binary)],
                                   text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(binary)], text=True, capture_output=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn(expected, run.stdout)

    def test_typed_object_promotion(self):
        source = (ROOT / 'port/linux/game/network_objects.c').read_text()
        code = PREAMBLE + '\n' + declaration(source, 'struct migration_object_state\n')
        for name in ('long network_objects_migration_write(',
                     'boolean network_objects_migration_validate(',
                     'static boolean network_objects_migration_weapon_local(',
                     'boolean network_objects_migration_restore('):
            code += '\n' + declaration(source, name)
        code += '\n' + MAIN
        self.compile_and_run(code, 'typed object handover passed')

    def test_engine_timers_without_score_rollback(self):
        source = (ROOT / 'source/game/game_engine.c').read_text()
        code = ENGINE_PREAMBLE + '\n' + declaration(source, 'struct game_engine_migration_state\n')
        for name in ('long game_engine_write_migration_state(',
                     'boolean game_engine_validate_migration_state(',
                     'boolean game_engine_read_migration_state('):
            code += '\n' + declaration(source, name)
        code += '\n' + ENGINE_MAIN
        self.compile_and_run(code, 'authority timers and score preservation passed')

    def test_client_object_replay_after_late_reattach(self):
        source = (ROOT / 'port/linux/game/network_objects.c').read_text()
        code = RESYNC_PREAMBLE
        for name in ('void network_objects_migration_reset_transport(',
                     'void network_objects_client_tick(',
                     'void network_objects_handle_changes(',
                     'void network_objects_handle_synchronized('):
            code += '\n' + declaration(source, name)
        code += '\n' + RESYNC_MAIN
        self.compile_and_run(code, 'late survivor object replay passed')

    def test_membership_and_route_coherence(self):
        header = (ROOT / 'source/networking/network_migration.h').read_text()
        client = (ROOT / 'source/networking/network_client_manager.c').read_text()
        distributed = (ROOT / 'port/linux/game/network_distributed.c').read_text()
        code = MEMBERSHIP_PREAMBLE
        for marker in ('struct network_migration_machine\n', 'struct network_migration_membership\n'):
            code += '\n' + declaration(header, marker)
        code += '\n' + declaration(distributed, 'struct migration_checkpoint_state\n')
        code += '\n' + MEMBERSHIP_FIXTURE
        for name in ('void network_game_client_migration_routes(',
                     'boolean network_game_client_migration_validate_machines(',
                     'boolean network_game_client_migration_set_machines('):
            code += '\n' + declaration(client, name)
        code += '\n' + declaration(distributed, 'static boolean distributed_checkpoint_validate_ownership(')
        code += '\n' + MEMBERSHIP_MAIN
        self.compile_and_run(code, 'checkpoint membership and ownership passed')


RESYNC_PREAMBLE = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef unsigned char byte;
typedef unsigned short word;
typedef int boolean;
#define TRUE 1
#define FALSE 0
#define NONE (-1L)
#define MAXIMUM_TRACKED_OBJECTS 16
#define MAXIMUM_CLIENT_NEW_OBJECTS 16
#define CLIENT_READY_INTERVAL_TICKS 30
#define CLIENT_READY_MAXIMUM_INTERVAL_TICKS 120
#define CLIENT_RETRY_TICKS 330
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define DATUM_INDEX_TO_ABSOLUTE_INDEX(index) ((long)((index) & 0xffffUL))
enum { _object_change_create, _object_change_delete,
       _distributed_message_client_ready, _distributed_to_host_reliably };
struct distributed_message_header { word type; };
struct distributed_object_change { byte change; long object_index; };
static long objects_host_told[16], objects_host_resting_cursor;
static long objects_host_inventories[16], objects_host_vehicle_predictions[16];
static long objects_client_has[16], objects_client_ready_time;
static long objects_client_ready_interval = CLIENT_READY_INTERVAL_TICKS;
static long objects_client_failed_time = NONE, objects_client_retry_ticks = CLIENT_RETRY_TICKS;
static short objects_client_new_object_count;
static boolean objects_client_check_all, objects_client_ask_again;
static boolean objects_client_synchronized, objects_client_resynchronizing;
static boolean objects_client_resync_seen[16];
static long objects_client_creating_index;
static boolean objects_client_creating, objects_client_deleting;
static long world[16], tick, requests, removed, vehicles;
static boolean client = TRUE;
static boolean network_game_distributed_client(void) { return client; }
static boolean distributed_object_index_valid(long index) { return index != NONE; }
static long game_time_get(void) { return tick; }
static void distributed_send(void *message, int type, int count, word size, int target) {
 (void)message;
 assert(type == _distributed_message_client_ready && count == 0);
 assert(size == sizeof(struct distributed_message_header) && target == _distributed_to_host_reliably);
 requests++;
}
static void distributed_client_remove_own_objects(void) { removed++; }
static void distributed_client_note_own_inventories(void) {}
static void distributed_client_send_identity(void) {}
static void distributed_client_send_vehicles(void) { vehicles++; }
static struct { long deletes; } objects_statistics;
static void distributed_client_delete(long index) {
 long slot = DATUM_INDEX_TO_ABSOLUTE_INDEX(index);
 assert(world[slot] == index); world[slot] = NONE;
}
static void distributed_client_create(struct distributed_object_change const *change) {
 long slot = DATUM_INDEX_TO_ABSOLUTE_INDEX(change->object_index);
 world[slot] = objects_client_has[slot] = change->object_index;
}
'''

RESYNC_MAIN = r'''
int main(void) {
 long survivor_unit = 0x10001L, missed_spawn = 0x20002L, deleted_item = 0x10003L;
 struct distributed_object_change replay[] = {
  { _object_change_create, survivor_unit }, { _object_change_create, missed_spawn }
 };
 for (int i = 0; i < 16; i++) world[i] = objects_client_has[i] = NONE;
 world[1] = objects_client_has[1] = survivor_unit;
 world[3] = objects_client_has[3] = deleted_item;
 objects_client_synchronized = TRUE;
 objects_client_ready_time = 100;
 tick = 200;
 network_objects_migration_reset_transport();
 /* No restart or eager world deletion; old canonical objects stay protected. */
 assert(world[1] == survivor_unit && world[3] == deleted_item);
 assert(objects_client_has[1] == survivor_unit && objects_client_has[3] == deleted_item);
 network_objects_client_tick();
 assert(requests == 1 && removed == 0);
 tick += 29;
 network_objects_client_tick();
 assert(requests == 1);
 tick++;
 network_objects_client_tick();
 assert(requests == 2 && removed == 0); /* Lost first request is retried. */
 network_objects_handle_changes(replay, 2);
 assert(world[1] == survivor_unit && objects_client_has[2] == missed_spawn);
 assert(world[3] == deleted_item); /* Wait for the complete reliable replay. */
 network_objects_handle_synchronized();
 assert(world[1] == survivor_unit && world[2] == missed_spawn && world[3] == NONE);
 assert(objects_client_has[3] == NONE && objects_client_synchronized);
 network_objects_client_tick();
 assert(requests == 2 && removed == 1);
 /* A second repair repeats reconciliation, without retaining earlier seen bits. */
 network_objects_migration_reset_transport();
 network_objects_handle_changes(replay, 1);
 network_objects_handle_synchronized();
 assert(world[1] == survivor_unit && world[2] == NONE);
 /* A promoted authority reannounces objects without entering client replay. */
 client = FALSE;
 network_objects_migration_reset_transport();
 assert(!objects_client_resynchronizing && objects_client_synchronized);
 puts("late survivor object replay passed");
}
'''

MEMBERSHIP_PREAMBLE = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef unsigned char byte;
typedef unsigned char boolean;
#define TRUE 1
#define FALSE 0
#define NONE (-1L)
#define HALO_PORT_MAXIMUM_NETWORK_MACHINES 128
#define csmemcpy memcpy
'''

MEMBERSHIP_FIXTURE = r'''
struct network_game_client {
 struct {
  short machine_count;
  struct network_migration_machine machines[HALO_PORT_MAXIMUM_NETWORK_MACHINES];
  long game_tick, unit_index;
 } game;
};
static struct network_game_client client;
static struct { unsigned long owner_addresses[HALO_PORT_MAXIMUM_NETWORK_MACHINES]; } client_migration;
static struct network_game_client *global_network_game_client_get(void) { return &client; }
'''

MEMBERSHIP_MAIN = r'''
int main(void) {
 struct migration_checkpoint_state state = {0};
 struct network_migration_membership before;
 long index;
 for (index = 0; index < HALO_PORT_MAXIMUM_NETWORK_MACHINES; index++)
  state.membership.machines[index].machine_index = NONE;
 state.membership.machine_count = 2;
 state.host_machine_index = 1;
 state.membership.machines[0].machine_index = 0;
 state.membership.machines[1].machine_index = 1;
 memset(state.membership.machines[0].name, 0x5a, sizeof(state.membership.machines[0].name));
 memset(state.membership.machines[1].name, 0x77, sizeof(state.membership.machines[1].name));
 state.machine_addresses[0] = 0x0a000042UL;
 state.machine_addresses[1] = 0x0a000099UL;
 assert(distributed_checkpoint_validate_ownership(&state));
 client.game.game_tick = 31877;
 client.game.unit_index = 0x123403;
 client.game.machines[0].machine_index = NONE;
 assert(network_game_client_migration_set_machines(&state.membership, sizeof(state.membership)));
 network_game_client_migration_routes(state.machine_addresses, HALO_PORT_MAXIMUM_NETWORK_MACHINES);
 assert(client.game.machine_count == 2 && client.game.machines[0].machine_index == 0);
 assert(!memcmp(client.game.machines[0].name, state.membership.machines[0].name, 0x40));
 assert(!memcmp(client.game.machines[1].name, state.membership.machines[1].name, 0x40));
 assert(client.game.game_tick == 31877 && client.game.unit_index == 0x123403);
 assert(client_migration.owner_addresses[0] == 0x0a000042UL);
 state.machine_addresses[2] = 0x0a000033UL;
 assert(!distributed_checkpoint_validate_ownership(&state));
 state.machine_addresses[2] = 0;
 state.machine_addresses[0] = 0;
 assert(!distributed_checkpoint_validate_ownership(&state));
 state.machine_addresses[0] = state.machine_addresses[1];
 assert(!distributed_checkpoint_validate_ownership(&state));
 state.machine_addresses[0] = 0x0a000042UL;
 state.host_machine_index = 2;
 assert(!distributed_checkpoint_validate_ownership(&state));
 state.host_machine_index = 1;
 before = state.membership;
 state.membership.machine_count = 3;
 assert(!distributed_checkpoint_validate_ownership(&state));
 assert(!network_game_client_migration_set_machines(&state.membership, sizeof(state.membership)));
 assert(!memcmp(client.game.machines, before.machines, sizeof(before.machines)));
 state.membership.machine_count = 2;
 state.membership.machines[0].machine_index = 2;
 assert(!distributed_checkpoint_validate_ownership(&state));
 assert(!network_game_client_migration_set_machines(&state.membership, sizeof(state.membership)));
 state.membership.machines[0].machine_index = 0;
 assert(!network_game_client_migration_validate_machines(&state.membership, sizeof(state.membership) - 1));
 assert(distributed_checkpoint_validate_ownership(&state));
 puts("checkpoint membership and ownership passed");
 return 0;
}
'''


PREAMBLE = r'''
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef unsigned char byte;
typedef unsigned char boolean;
typedef unsigned short word;
#define TRUE 1
#define FALSE 0
#define NONE (-1L)
#define NO_PLAYER 255
#define MAXIMUM_TRACKED_PLAYERS 16
#define MAXIMUM_TRACKED_OBJECTS 16
#define MAXIMUM_WEAPONS_PER_UNIT 4
#define MAXIMUM_REGIONS_PER_OBJECT 8
#define NUMBER_OF_OBJECT_CHANGE_COLORS 4
#define FLAG(n) (1UL << (n))
#define TEST_FLAG(value, bit) (!!((value) & FLAG(bit)))
#define DATUM_INDEX_TO_ABSOLUTE_INDEX(index) ((long)((index) & 0xffffUL))
enum { _object_type_biped, _object_type_vehicle, _object_type_weapon,
       _object_type_equipment, _object_type_projectile, NUMBER_OF_OBJECT_TYPES };
enum { _object_header_being_deleted_bit = 0 };
#define _object_mask_item (FLAG(_object_type_weapon) | FLAG(_object_type_equipment))
#define _object_mask_projectile FLAG(_object_type_projectile)
#define _object_mask_unit (FLAG(_object_type_biped) | FLAG(_object_type_vehicle))
#define NETWORKED_OBJECT_TYPES (FLAG(_object_type_biped) | FLAG(_object_type_vehicle) | _object_mask_item)
typedef struct { float x, y, z; } real_point3d;
typedef struct { float i, j, k; } real_vector3d;
typedef struct { float r, g, b; } real_rgb_color;
struct _item_datum { unsigned long flags; short detonation_ticks; long last_owned_time; };
struct weapon_magazine { short state, state_timer, rounds_total, rounds_loaded; };
struct _weapon_datum {
 unsigned long flags;
 float heat;
 long tracked_object_index, overheated_effect_index;
 struct { long charging_effect_index; } triggers[2];
 struct weapon_magazine magazines[2];
};
struct _projectile_datum {
 unsigned long flags;
 long ignore_object_index, target_object_index, tracer_attachment_index;
 float detonation_timer, detonation_timer_delta, arming_time, arming_time_delta;
};
struct _object_datum {
 short type;
 long shield_damage_decay_timer, body_damage_decay_timer;
 short shield_stun_ticks;
 long owner_object_index, parent_object_index;
 short parent_node_index;
 real_point3d position;
 real_vector3d translational_velocity, forward, up, angular_velocity;
};
struct object_datum { long definition_index; struct _object_datum object; };
struct item_datum { long definition_index; struct _object_datum object; struct _item_datum item; };
struct weapon_datum { long definition_index; struct _object_datum object; struct _item_datum item; struct _weapon_datum weapon; };
struct projectile_datum { long definition_index; struct _object_datum object; struct _item_datum item; struct _projectile_datum projectile; };
struct unit_datum { long definition_index; struct _object_datum object; struct { long player_index; } unit; };
union fixture_object { struct weapon_datum weapon; struct projectile_datum projectile; struct unit_datum unit; };
struct datum_header { short identifier; };
struct object_header_datum { short identifier; void *datum; unsigned long flags; };
struct data_array { void *data; long size; short maximum_count; };
struct object_iterator { long index, cursor; unsigned long mask; };
struct distributed_object_change {
 byte change, flags, owner_player_index, pad;
 long object_index, definition_index;
 short owner_team_index, variant_number;
 real_point3d position;
 real_vector3d forward, up, translational_velocity, angular_velocity;
 real_rgb_color change_colors[NUMBER_OF_OBJECT_CHANGE_COLORS];
 byte region_permutations[MAXIMUM_REGIONS_PER_OBJECT];
};
static union fixture_object world[16];
static struct object_header_datum headers[16];
static struct data_array object_header_array = { headers, sizeof(headers[0]), 16 };
static struct data_array *object_header_data = &object_header_array;
static int creates, deletes, projectile_steps, replaying, damage;
static long ids[16];
static struct object_datum *object_try_and_get(long index) {
 long slot = DATUM_INDEX_TO_ABSOLUTE_INDEX(index);
 if (index == NONE || slot >= 16 || !headers[slot].identifier || ids[slot] != index) return NULL;
 return (struct object_datum *)&world[slot];
}
static struct object_header_datum *object_header_try_and_get(long index) {
 return object_try_and_get(index) ? &headers[DATUM_INDEX_TO_ABSOLUTE_INDEX(index)] : NULL;
}
static struct object_datum *object_try_and_get_and_verify_type(long index, unsigned long mask) {
 struct object_datum *object = object_try_and_get(index);
 return object && TEST_FLAG(mask, object->object.type) ? object : NULL;
}
static boolean distributed_player_is_local(long player) { return player == 0; }
static void spawn(short slot, short generation, short type, long definition) {
 memset(&world[slot], 0, sizeof(world[slot]));
 headers[slot].identifier = generation;
 headers[slot].datum = &world[slot];
 headers[slot].flags = 0;
 ids[slot] = ((long)generation << 16) | slot;
 world[slot].weapon.definition_index = definition;
 world[slot].weapon.object.type = type;
 world[slot].weapon.object.owner_object_index = NONE;
 world[slot].weapon.object.parent_object_index = NONE;
 if (type == _object_type_biped || type == _object_type_vehicle) world[slot].unit.unit.player_index = NONE;
}
static void object_iterator_new(struct object_iterator *it, unsigned long mask, int flags) {
 (void)flags; it->cursor = 0; it->mask = mask;
}
static struct object_datum *object_iterator_next(struct object_iterator *it) {
 while (it->cursor < 16) {
  long slot = it->cursor++;
  if (!headers[slot].identifier || !TEST_FLAG(it->mask, world[slot].weapon.object.type)) continue;
  it->index = ids[slot]; return (struct object_datum *)&world[slot];
 }
 return NULL;
}
static void distributed_change_from_object(long index, struct distributed_object_change *change) {
 struct object_datum *object = object_try_and_get(index);
 memset(change, 0, sizeof(*change));
 change->object_index = index; change->definition_index = object->definition_index;
 change->owner_player_index = NO_PLAYER;
 change->position = object->object.position;
 change->translational_velocity = object->object.translational_velocity;
}
static void object_get_origin(long index, real_point3d *out) { *out = object_try_and_get(index)->object.position; }
static void object_get_orientation(long index, real_vector3d *forward, real_vector3d *up) {
 *forward = object_try_and_get(index)->object.forward; *up = object_try_and_get(index)->object.up;
}
static void distributed_client_delete(long index) {
 long slot = DATUM_INDEX_TO_ABSOLUTE_INDEX(index);
 assert(object_try_and_get(index)); headers[slot].identifier = 0; headers[slot].datum = NULL; deletes++;
}
static void distributed_client_create(struct distributed_object_change const *change) {
 long slot = DATUM_INDEX_TO_ABSOLUTE_INDEX(change->object_index);
 assert(!headers[slot].identifier);
 spawn((short)slot, (short)(change->object_index >> 16), _object_type_projectile, change->definition_index);
 world[slot].projectile.object.position = change->position;
 world[slot].projectile.object.translational_velocity = change->translational_velocity;
 creates++;
}
static void object_attach_to_node(long parent, long child, short node) {
 struct object_datum *object = object_try_and_get(child);
 assert(object_try_and_get(parent)); object->object.parent_object_index = parent; object->object.parent_node_index = node;
}
static void network_damage_migration_replay(boolean active) { replaying = active; }
static boolean projectile_update(long index) {
 struct projectile_datum *projectile = (struct projectile_datum *)object_try_and_get(index);
 assert(replaying); projectile_steps++;
 projectile->object.position.x += projectile->object.translational_velocity.i;
 projectile->projectile.arming_time += projectile->projectile.arming_time_delta;
 projectile->projectile.detonation_timer += projectile->projectile.detonation_timer_delta;
 if (!replaying) damage++;
 if (projectile->projectile.detonation_timer >= 1) distributed_client_delete(index);
 return TRUE;
}
'''

MAIN = r'''
int main(void) {
 byte snapshot[16384], malformed[16384];
 long size;
 int old_creates;
 struct migration_object_state record;
 spawn(1, 3, _object_type_biped, 101);
 spawn(2, 4, _object_type_weapon, 102);
 world[2].weapon.weapon.heat = .75f;
 world[2].weapon.weapon.tracked_object_index = NONE;
 world[2].weapon.weapon.magazines[0].rounds_total = 21;
 world[2].weapon.weapon.magazines[0].rounds_loaded = 6;
 world[2].weapon.weapon.magazines[0].state_timer = 9;
 world[2].weapon.weapon.magazines[0].state = 1; /* snapshot was reloading */
 world[2].weapon.weapon.flags = 4; /* snapshot requested reload */
 world[2].weapon.item.last_owned_time = 1234;
 spawn(4, 8, _object_type_weapon, 104);
 world[4].weapon.weapon.heat = .9f;
 world[4].weapon.weapon.tracked_object_index = NONE;
 world[4].weapon.weapon.magazines[0].rounds_loaded = 12;
 world[4].weapon.weapon.magazines[0].state_timer = 7;
 spawn(3, 5, _object_type_projectile, 103);
 world[3].projectile.object.owner_object_index = ids[1];
 world[3].projectile.object.position.x = 10;
 world[3].projectile.object.translational_velocity.i = 2;
 world[3].projectile.projectile.detonation_timer = .2f;
 world[3].projectile.projectile.detonation_timer_delta = .1f;
 world[3].projectile.projectile.arming_time_delta = .15f;
 world[3].projectile.projectile.ignore_object_index = ids[1];
 world[3].projectile.projectile.target_object_index = NONE;
 size = network_objects_migration_write(snapshot, sizeof(snapshot));
 assert(size > 0 && network_objects_migration_validate(snapshot, size));
 assert(!network_objects_migration_validate(snapshot, size - 1));
 memcpy(malformed, snapshot, size);
 memcpy(&record, malformed + sizeof(long), sizeof(record));
 record.identity.object_index = 0xffff;
 memcpy(malformed + sizeof(long), &record, sizeof(record));
 assert(!network_objects_migration_validate(malformed, size));
 /* A subsequent inventory update spent two rounds. Handover must not refund them. */
 world[2].weapon.weapon.magazines[0].rounds_total = 19;
 world[2].weapon.weapon.magazines[0].rounds_loaded = 4;
 world[2].weapon.weapon.heat = .1f;
 world[2].weapon.weapon.magazines[0].state_timer = 2;
 world[2].weapon.weapon.magazines[0].state = 0; /* current filled clip is idle */
 world[2].weapon.weapon.flags = 0;
 world[2].weapon.item.flags = 1;
 world[4].weapon.weapon.heat = .2f;
 spawn(8, 7, _object_type_projectile, 108); /* local predicted generation */
 assert(network_objects_migration_restore(snapshot, size, 2));
 assert(headers[1].identifier == 3 && headers[2].identifier == 4);
 assert(world[2].weapon.weapon.magazines[0].rounds_total == 19);
 assert(world[2].weapon.weapon.magazines[0].rounds_loaded == 4);
 assert(world[2].weapon.weapon.magazines[0].state_timer == 2);
 assert(world[2].weapon.weapon.magazines[0].state == 0 && world[2].weapon.weapon.flags == 0);
 assert(world[2].weapon.weapon.heat == .1f && world[2].weapon.item.last_owned_time == 1234);
 assert(world[2].weapon.item.flags == 1);
 assert(world[4].weapon.weapon.heat == .9f && world[4].weapon.weapon.magazines[0].state_timer == 7);
 /* A local player's tuple wins even when current and checkpoint ammo match. */
 world[1].unit.unit.player_index = 0;
 world[4].weapon.object.parent_object_index = ids[1];
 world[4].weapon.weapon.heat = .2f;
 world[4].weapon.weapon.magazines[0].state_timer = 1;
 assert(!headers[8].identifier && headers[3].identifier == 5);
 assert(world[3].projectile.object.position.x == 14);
 assert(world[3].projectile.object.owner_object_index == ids[1]);
 assert(world[3].projectile.projectile.arming_time > .29f);
 assert(projectile_steps == 2 && damage == 0 && !replaying);
 /* A newer canonical generation occupies an old projectile's slot. It wins. */
 distributed_client_delete(ids[3]);
 spawn(3, 6, _object_type_biped, 203);
 old_creates = creates;
 assert(network_objects_migration_restore(snapshot, size, 2));
 assert(headers[3].identifier == 6 && world[3].weapon.definition_index == 203);
 assert(creates == old_creates && damage == 0);
 assert(world[4].weapon.weapon.heat == .2f && world[4].weapon.weapon.magazines[0].state_timer == 1);
 /* Invalid payloads perform no destructive work. */
 old_creates = deletes;
 assert(!network_objects_migration_restore(malformed, size, 2));
 assert(deletes == old_creates);
 puts("typed object handover passed");
 return 0;
}
'''

ENGINE_PREAMBLE = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef unsigned char byte;
typedef unsigned char boolean;
typedef float real;
#define FALSE 0
#define TRUE 1
static int engine_object;
static void *game_engine = &engine_object;
static long engine_type = 3;
static long score = 18;
static struct {
 unsigned long flags;
 long next_team_index;
 real postgame_timer, postgame_progress;
 long postgame_state;
 real hud_message_timers[4];
} game_engine_globals;
static long game_engine_get_type(void) { return engine_type; }
static long game_engine_write_network_state(byte *buffer, long size) {
 if (size < (long)sizeof(score)) return 0;
 memcpy(buffer, &score, sizeof(score)); return sizeof(score);
}
static void game_engine_read_network_state(byte const *buffer, long size) {
 assert(size == sizeof(score)); memcpy(&score, buffer, sizeof(score));
}
'''

ENGINE_MAIN = r'''
int main(void) {
 byte buffer[1024];
 long size;
 game_engine_globals.flags = 21;
 game_engine_globals.next_team_index = 9;
 game_engine_globals.postgame_timer = 12.5f;
 game_engine_globals.postgame_progress = .25f;
 size = game_engine_write_migration_state(buffer, sizeof(buffer));
 assert(size > 0 && game_engine_validate_migration_state(buffer, size));
 score = 25; /* A reliable score update arrived after the checkpoint. */
 game_engine_globals.postgame_timer = 0;
 game_engine_globals.hud_message_timers[0] = 3.5f;
 assert(game_engine_read_migration_state(buffer, size, FALSE));
 assert(score == 25 && game_engine_globals.postgame_timer == 12.5f);
 assert(game_engine_globals.next_team_index == 9 && game_engine_globals.flags == 21);
 assert(game_engine_globals.hud_message_timers[0] == 3.5f);
 assert(!game_engine_read_migration_state(buffer, size - 1, TRUE));
 assert(score == 25 && game_engine_globals.postgame_timer == 12.5f);
 engine_type = 4;
 assert(!game_engine_read_migration_state(buffer, size, TRUE));
 assert(score == 25);
	engine_type = 3;
	game_engine_globals.postgame_state = 1;
	game_engine_globals.postgame_timer = 8;
	assert(game_engine_read_migration_state(buffer, size, FALSE));
	assert(game_engine_globals.postgame_state == 1 && game_engine_globals.postgame_timer == 8);
	assert(game_engine_read_migration_state(buffer, size, TRUE) && score == 18);
 puts("authority timers and score preservation passed");
 return 0;
}
'''


if __name__ == '__main__':
    unittest.main()
