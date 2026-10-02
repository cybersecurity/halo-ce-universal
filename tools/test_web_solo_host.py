"""Exercise the real last-team predicate with retained departed-player datums.

Browser distributed FFA Slayer hosts must keep waiting for late joiners while
the original native, team, elimination and explicit game-end behavior remains.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
SOURCE = (ROOT / "source/game/game_engine.c").read_text()


def function(name):
    match = re.search(r"(?:boolean|void|long) " + name + r"\([^;]*?\)\s*\{", SOURCE)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        end += 1
    return SOURCE[match.start():end]


BOUNDARY = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef int boolean;
#define TRUE 1
#define FALSE 0
#define NONE (-1)
#define match_assert(file, line, condition) assert(condition)
enum { game_engine_ctf = 1, game_engine_slayer, game_engine_oddball, game_engine_king, game_engine_race };
enum { game_engine_mode_active, game_engine_mode_postgame_delay };
enum { _multiplayer_sound_game_over = 1 };
struct player_datum { boolean quit_out_of_game; long unit_index, team_index; };
struct data_iterator { int next; long datum_index; };
static struct player_datum players[20];
static int player_count, distributed, hosting, explicit_sounds, widget_closes;
static struct game_engine { long type; } engine, *game_engine = &engine;
static struct network_game_server { int unused; } server;
static struct { struct { boolean teams; long lives; } universal_variant; } global_variant;
static struct { int postgame_state; float postgame_timer; } game_engine_globals;
static void *player_data;
static void data_iterator_new(struct data_iterator *iterator, void *data) {
    (void)data; iterator->next = 0; iterator->datum_index = NONE;
}
static void *data_iterator_next(struct data_iterator *iterator) {
    if (iterator->next == player_count) return NULL;
    iterator->datum_index = iterator->next;
    return &players[iterator->next++];
}
static boolean game_engine_player_is_odd_man_out(long index) { (void)index; return FALSE; }
static boolean game_engine_player_is_out_of_lives(long index) { (void)index; return FALSE; }
static struct network_game_server *global_network_game_server_get(void) { return hosting ? &server : NULL; }
static boolean network_game_distributed(void) { return distributed; }
static void game_engine_play_multiplayer_sound(int sound) { assert(sound == 1); explicit_sounds++; }
static void ui_widgets_close_all(void) { widget_closes++; }
'''

FUNCTIONS = "\n".join(function(name) for name in (
    "players_in_game", "multiple_teams_alive", "game_engine_should_end_game", "game_engine_end_game"))

CASES = r'''
static void reset(void) {
    memset(players, 0, sizeof(players));
    memset(&global_variant, 0, sizeof(global_variant));
    memset(&game_engine_globals, 0, sizeof(game_engine_globals));
    game_engine = &engine; engine.type = game_engine_slayer;
    hosting = distributed = TRUE; player_count = 1;
    players[0] = (struct player_datum){ FALSE, 10, 0 };
    explicit_sounds = widget_closes = 0;
}

int main(void) {
    reset();
    assert(!game_engine_should_end_game()); /* initial solo host */
    players[1] = (struct player_datum){ FALSE, 11, 1 }; player_count = 2;
    assert(!game_engine_should_end_game()); /* opponent present */
    players[1].quit_out_of_game = TRUE;
    assert(players_in_game() == 2 && !multiple_teams_alive());
#ifdef HALO_WEB
    assert(!game_engine_should_end_game()); /* opponent reloads/leaves */
#else
    assert(game_engine_should_end_game()); /* original native behavior */
#endif
    /* Many retained score datums do not turn a waiting browser host into
       a last-player win. Their data is never deleted by this predicate. */
    for (int i = 2; i < 20; i++) players[i] = (struct player_datum){ TRUE, NONE, i };
    player_count = 20;
#ifdef HALO_WEB
    assert(!game_engine_should_end_game());
#else
    assert(game_engine_should_end_game());
#endif
    assert(players_in_game() == 20 && players[0].unit_index == 10);
    /* The score/time/explicit finish path still starts its seven-second
       postgame sequence, even when the last-team predicate stays false. */
    game_engine_end_game();
    assert(game_engine_globals.postgame_state == 1 && game_engine_globals.postgame_timer == 7.0f);
    assert(explicit_sounds == 1 && widget_closes == 1);
    game_engine_end_game(); assert(explicit_sounds == 1 && widget_closes == 1);

    global_variant.universal_variant.teams = TRUE;
    assert(game_engine_should_end_game()); global_variant.universal_variant.teams = FALSE;
    global_variant.universal_variant.lives = 1;
    assert(game_engine_should_end_game()); global_variant.universal_variant.lives = 0;
    distributed = FALSE; assert(game_engine_should_end_game()); distributed = TRUE;
    hosting = FALSE; assert(game_engine_should_end_game()); hosting = TRUE;
    for (int type = game_engine_ctf; type <= game_engine_race; type++) {
        if (type == game_engine_slayer) continue;
        engine.type = type; assert(game_engine_should_end_game());
    }
    game_engine = NULL; assert(!game_engine_should_end_game());
    puts("Solo browser waiting, opponent departure, native/team/elimination and explicit game-end gates passed.");
}
'''

with tempfile.TemporaryDirectory(prefix="halo-solo-host-") as directory:
    path = Path(directory)

    def run(name, functions, web):
        source = path / f"{name}.c"
        source.write_text(BOUNDARY + functions + CASES)
        output = path / name
        subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-function", "-fsanitize=address,undefined",
                        *(["-DHALO_WEB=1"] if web else []), str(source), "-o", str(output)], check=True)
        return subprocess.run([str(output)], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    for web in (False, True):
        result = run("web" if web else "native", FUNCTIONS, web)
        assert result.returncode == 0, result.stderr
        print(result.stdout.strip())
    mutant, count = re.subn(r"#ifdef HALO_WEB\n.*?#endif\n", "", FUNCTIONS, flags=re.S)
    assert count == 1
    original = run("original-last-team-control", mutant, True)
    assert original.returncode != 0 and "Assertion" in original.stderr, original.stderr
    print("Original last-team departure behavior rejected by the browser regression.")
