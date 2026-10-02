"""Run production player creation/removal with a bounded datum allocator.

Departed players remain allocated for scoring, but their machine slots must be
available to a replacement connection. This exercises the native lifecycle,
without a renderer, and checks controls that reproduce the original failures.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def function(path, name):
    source = (ROOT / path).read_text()
    match = re.search(r"(?:static )?(?:boolean|void|long|long \*)\s*" + name + r"\([^;]*?\)\s*\{", source)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


BOUNDARY = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
typedef int boolean;
typedef unsigned char byte;
typedef unsigned short word;
#define HALO_LINUX 1
#define TRUE 1
#define FALSE 0
#define NONE (-1)
#define MAXIMUM_NETWORK_MACHINE_COUNT 128
#define MAXIMUM_LOCAL_PLAYERS 4
#define MAXIMUM_NUMBER_OF_LOCAL_PLAYERS 4
#define MAXIMUM_NUMBER_OF_PLAYERS 128
#define NETWORK_GAME_PLAYER_SLOTS 128
#define _player_action_result_reload 0
#define _error_silent 0
#define DATUM_INDEX_TO_ABSOLUTE_INDEX(index) ((long)(index) & 0xffff)
#define VALID_INDEX(index, count) ((index) >= 0 && (index) < (count))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define match_assert(file, line, condition) assert(condition)
#define csmemcpy memcpy
#define ustrncpy wcsncpy
#define error(...) ((void)0)
#define network_event(...) ((void)0)
struct network_player {
    wchar_t name[12]; int machine_index, controller_index, team_index, player_list_index;
};
struct player_datum {
    short identifier, local_player_index;
    wchar_t name[12];
    long unit_index, dead_unit_index, squad_index, cluster_index, aim_assist_unit_index;
    float speed_multiplier; long team_index, action_result, action_object_index;
    long quit_out_of_game_time; boolean quit_out_of_game;
    struct network_player network_player_data;
};
struct datum_header { short identifier; };
struct data_array { short next_identifier, maximum_count; long size; boolean valid; void *data; };
struct network_game {
    int player_count, maximum_players;
    struct network_player players[128];
    struct { boolean game_objects_loaded; } local_data;
};
struct network_game_client { struct network_game game; int machine_index; };
static struct player_datum datums[128];
static struct data_array array = { 0x100, 128, sizeof(struct player_datum), TRUE, datums };
static struct data_array *player_data = &array;
static long machine_to_player_table[128][4];
static boolean distributed = TRUE;
static int local_quits, machine_overflows, allocation_fails;
static boolean network_game_distributed(void) { return distributed; }
static boolean game_in_progress(void) { return TRUE; }
static boolean game_engine_running(void) { return TRUE; }
static long game_time_get(void) { return 100; }
static void machine_remove_player(long index) { (void)index; }
static long datum_new_at_index(struct data_array *data, long index) {
    long slot = DATUM_INDEX_TO_ABSOLUTE_INDEX(index);
    if (allocation_fails || slot >= data->maximum_count || datums[slot].identifier) return NONE;
    memset(&datums[slot], 0, sizeof(datums[slot]));
    datums[slot].identifier = (short)((index >> 16) & 0xffff);
    data->next_identifier++;
    return index;
}
static long datum_new(struct data_array *data) {
    for (long i = 0; i < data->maximum_count; i++) {
        if (!datums[i].identifier)
            return datum_new_at_index(data, ((long)data->next_identifier << 16) | i);
    }
    return NONE;
}
static struct player_datum *player_get(long index) {
    long slot = DATUM_INDEX_TO_ABSOLUTE_INDEX(index);
    assert(slot < 128 && datums[slot].identifier == ((index >> 16) & 0xffff));
    return &datums[slot];
}
static long unstrip_player_index(long slot) {
    return slot >= 0 && slot < 128 && datums[slot].identifier ?
        ((long)(word)datums[slot].identifier << 16) | slot : NONE;
}
static boolean network_game_player_is_local(struct network_player *player) {
    return player->machine_index == 0;
}
static void network_game_client_all_local_players_have_quit(void) { local_quits++; }
static void display_assert(const char *message, const char *file, int line, int fatal) {
    (void)message; (void)file; (void)line; (void)fatal; machine_overflows++;
}
static void system_exit(int status) { (void)status; }
'''


PLAYERS = "source/game/players.c"
MANAGER = "source/networking/network_game_manager.c"
CLIENT = "source/networking/network_client_manager.c"
FUNCTIONS = "\n".join((
    function(PLAYERS, "machine_get_player_list"),
    function(PLAYERS, "network_player_remove_from_machine"),
    function(PLAYERS, "machine_add_player"),
    function(PLAYERS, "player_new"),
    function(MANAGER, "network_player_is_valid"),
    function(MANAGER, "network_game_player_slot_held"),
    function(MANAGER, "network_game_add_player"),
    function(MANAGER, "network_game_spawn_player"),
    function(MANAGER, "network_game_player_is_valid"),
    function(MANAGER, "network_game_invalidate_player"),
    function(MANAGER, "network_game_remove_player"),
    function(CLIENT, "network_game_client_remove_player"),
))


CASES = r'''
static void reset(struct network_game_client *client) {
    memset(client, 0, sizeof(*client));
    memset(datums, 0, sizeof(datums));
    memset(machine_to_player_table, 0xff, sizeof(machine_to_player_table));
    array.next_identifier = 0x100;
    client->machine_index = 0;
    client->game.maximum_players = 128;
    client->game.local_data.game_objects_loaded = TRUE;
    for (int i = 0; i < 128; i++) network_game_invalidate_player(&client->game.players[i]);
    local_quits = machine_overflows = allocation_fails = 0;
    distributed = TRUE;
}

static long join(struct network_game_client *client, int machine, int controller) {
    struct network_player player = { L"Player", machine, controller, 0, NONE };
    assert(network_game_add_player(&client->game, &player));
    assert(network_game_spawn_player(&client->game.players[player.player_list_index]));
    return unstrip_player_index(player.player_list_index);
}

int main(void) {
    struct network_game_client client;
    reset(&client);
    long host = join(&client, 0, 0);
    long retained[80]; int total = 0;
    /* Four split-screen players reconnect twenty times on the same machine.
       Quit is scheduled for the next tick; reconnect happens before that tick.
       Their old score datums stay allocated, preventing identity reuse. */
    for (int generation = 0; generation < 20; generation++) {
        long active[4];
        for (int controller = 0; controller < 4; controller++) {
            active[controller] = join(&client, 1, controller);
            assert(machine_to_player_table[1][controller] == active[controller]);
        }
        assert(client.game.player_count == 5 && !machine_overflows);
        for (int controller = 0; controller < 4; controller++) {
            int slot = (int)DATUM_INDEX_TO_ABSOLUTE_INDEX(active[controller]);
            /* Also exercise an argument aliasing the settings record that
               removal invalidates; ownership uses the retained datum. */
            assert(network_game_client_remove_player(&client, &client.game.players[slot], 101));
            assert(machine_to_player_table[1][controller] == NONE);
            assert(player_get(active[controller])->quit_out_of_game_time == 101);
            assert(!player_get(active[controller])->quit_out_of_game);
            retained[total++] = active[controller];
        }
        assert(client.game.player_count == 1 && !local_quits);
        assert(machine_to_player_table[0][0] == host);
        for (int i = 0; i < total; i++) assert(player_get(retained[i])->quit_out_of_game_time == 101);
    }
    assert(total == 80 && !machine_overflows);

    /* No reason/timed death is needed to remove input ownership. */
    long remote = join(&client, 2, 0);
    int slot = (int)DATUM_INDEX_TO_ABSOLUTE_INDEX(remote);
    assert(network_game_client_remove_player(&client, &client.game.players[slot], NONE));
    assert(machine_to_player_table[2][0] == NONE && player_get(remote)->quit_out_of_game_time == NONE);

    /* Lockstep still retains its original mapping until the map ends. */
    remote = join(&client, 3, 0); distributed = FALSE;
    slot = (int)DATUM_INDEX_TO_ABSOLUTE_INDEX(remote);
    assert(network_game_client_remove_player(&client, &client.game.players[slot], 101));
    assert(machine_to_player_table[3][0] == remote); distributed = TRUE;

    /* Failed allocation must not enter a full machine list and terminate. */
    for (int i = 0; i < 4; i++) machine_to_player_table[4][i] = host;
    allocation_fails = TRUE;
    struct network_player rejected = { L"Player", 4, 0, 0, 100 };
    assert(player_new(4, ((long)array.next_identifier << 16) | 100, NONE, &rejected) == NONE);
    assert(!machine_overflows);
    for (int i = 0; i < 4; i++) assert(machine_to_player_table[4][i] == host);

    network_player_remove_from_machine(NONE, host);
    network_player_remove_from_machine(128, host);
    network_player_remove_from_machine(0, NONE);
    assert(machine_to_player_table[0][0] == host);
    puts("Repeated native rejoin, retained scores, ownership removal and allocation failure passed.");
}
'''


with tempfile.TemporaryDirectory(prefix="halo-player-rejoin-") as directory:
    path = Path(directory)

    def run(name, functions):
        source = path / f"{name}.c"
        source.write_text(BOUNDARY + functions + CASES)
        output = path / name
        subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-variable", "-fsanitize=address,undefined", str(source), "-o", str(output)], check=True)
        return subprocess.run([str(output)], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    fixed = run("fixed", FUNCTIONS)
    assert fixed.returncode == 0, fixed.stderr
    print(fixed.stdout.strip())
    for name, original, mutant in (
        ("missing-detach", "network_player_remove_from_machine(player_datum->network_player_data.machine_index, player_index);",
         "((void)0);"),
        ("failed-allocation-mapping", "if (player_index != NONE)\n\t\tmachine_add_player",
         "if (TRUE)\n\t\tmachine_add_player"),
    ):
        assert original in FUNCTIONS, name
        control = run(name, FUNCTIONS.replace(original, mutant))
        assert control.returncode != 0, f"{name}: original defect must fail the regression"
        assert "Assertion" in control.stderr, control.stderr
        print(f"Original-defect control rejected: {name}")
