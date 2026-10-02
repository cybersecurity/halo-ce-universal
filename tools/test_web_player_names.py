"""Check real browser player-name picking and host admission without a renderer."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def function(path, name):
    source = (ROOT / path).read_text()
    match = re.search(r"(?:static )?(?:boolean|void|wchar_t const \*)\s*" + name + r"\([^;]*?\)\s*\{", source)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


BOUNDARY = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
typedef int boolean;
#define HALO_WEB 1
#define HALO_LINUX 1
#define TRUE 1
#define FALSE 0
#define NONE (-1)
#define NUMBEROF(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define VALID_INDEX(index, count) ((index) >= 0 && (index) < (count))
#define MAXIMUM_NETWORK_PLAYER_COUNT 128
#define MAXIMUM_UNIQUE_NAME_ATTEMPTS 64
#define NETWORK_PLAYER_NAME_LENGTH 12
#define NUMBER_OF_MULTIPLAYER_TEAMS 128
#define match_assert(file, line, condition) assert(condition)
#define network_event(...) ((void)0)
#define csmemcpy memcpy
#define ustrncpy wcsncpy
#define ustrlen wcslen
#define ustrcmp wcscmp
struct network_player {
    wchar_t name[12]; int machine_index, controller_index, team_index, player_list_index, primary_color_index;
};
struct network_game { struct network_player players[128]; int count; };
struct network_game_server { struct network_game game; };
struct network_game_server_client_machine { int machine_index; };
struct string_list { struct { int count; } strings; };
static struct string_list list;
static int tag_available, definition_available, random_calls;
static unsigned int local_seed, gameplay_seed = 0x12345678;
static wchar_t const *tag_names[] = { L"ValidName", L" \t\n" };
static long tag_loaded(unsigned int group, char const *name) {
    (void)group; (void)name; return tag_available ? 1 : NONE;
}
static struct string_list *unicode_string_list_definition_get(long index) {
    (void)index; return definition_available ? &list : NULL;
}
static wchar_t const *unicode_string_list_get_string(long tag, short index) {
    (void)tag; assert(index >= 0 && index < list.strings.count); return tag_names[index];
}
static unsigned int *get_global_local_random_seed_address(void) { return &local_seed; }
static short seed_random_range(unsigned int *seed, int minimum, int maximum) {
    assert(seed == &local_seed && minimum >= 0 && maximum >= minimum);
    random_calls++; return (short)(minimum + (*seed)++ % (unsigned int)(maximum - minimum + 1));
}
static boolean network_player_is_valid(struct network_player *player) { return player->machine_index >= 0; }
static void get_unique_random_color(struct network_game_server *server, struct network_player *player) {
    (void)server; player->primary_color_index = 1;
}
static boolean network_game_add_player(struct network_game *game, struct network_player *player) {
    assert(game->count < 128 && player->player_list_index == NONE);
    player->player_list_index = game->count;
    game->players[game->count++] = *player;
    return TRUE;
}
'''

UI = "source/networking/network_game_ui.c"
SERVER = "source/networking/network_server_manager.c"
FUNCTIONS = "\n".join((
    function(UI, "network_game_player_name_is_blank"),
    function(UI, "network_game_get_random_player_name"),
    function(SERVER, "player_name_is_unique"),
    function(SERVER, "get_unique_random_name"),
    function(SERVER, "network_game_server_add_player_to_game"),
))
CASES = r'''
int main(void) {
    assert(network_game_player_name_is_blank(NULL));
    assert(network_game_player_name_is_blank(L""));
    assert(network_game_player_name_is_blank(L" \t\r\n"));
    assert(network_game_player_name_is_blank(L"\x85\xA0\x2003\x200B\x3000\xFEFF"));
    wchar_t bounded[11]; for (int i = 0; i < 11; i++) bounded[i] = L' ';
    assert(network_game_player_name_is_blank(bounded)); // No read past the network field.
    assert(!network_game_player_name_is_blank(L" Nova "));
    assert(!network_game_player_name_is_blank(L"\x200B" L"Nova"));
    assert(!network_game_player_name_is_blank(L"ABCDEFGHIJK"));

    /* Missing tags, missing definitions, empty lists and blank tag strings
       must all produce useful names. Valid loaded names stay available. */
    for (int scenario = 0; scenario < 5; scenario++) {
        tag_available = scenario != 0; definition_available = scenario != 1;
        list.strings.count = scenario == 2 ? 0 : 2;
        if (scenario == 3) tag_names[0] = L"";
        if (scenario == 4) tag_names[0] = L"ValidName";
        for (int i = 0; i < 40; i++) {
            wchar_t const *name = network_game_get_random_player_name();
            assert(!network_game_player_name_is_blank(name));
            assert(wcslen(name) <= 11);
        }
    }
    tag_available = definition_available = 1; list.strings.count = 1;
    tag_names[0] = L"ValidName";
    assert(!wcscmp(network_game_get_random_player_name(), L"ValidName"));

    /* Admission while playing has no UI tag: all 128 names must be nonblank,
       bounded and unique, even after the built-in pool is exhausted. */
    struct network_game_server server = { 0 };
    for (int i = 0; i < 128; i++) server.game.players[i].machine_index = NONE;
    tag_available = 0;
    for (int i = 0; i < 128; i++) {
        struct network_player player = { L"", i, 0, 0, NONE, NONE };
        struct network_game_server_client_machine machine = { i };
        if (i == 0) wcscpy(player.name, L" MyName ");
        if (i % 3 == 1) wcscpy(player.name, L" \t\xA0\x200B");
        assert(network_game_server_add_player_to_game(&server, &machine, &player));
        assert(!network_game_player_name_is_blank(player.name));
        assert(wcslen(player.name) <= 11 && player.name[11] == 0);
        assert(!wcscmp(player.name, server.game.players[i].name));
        if (i == 0) assert(!wcscmp(player.name, L" MyName "));
        for (int previous = 0; previous < i; previous++)
            assert(wcscmp(player.name, server.game.players[previous].name));
    }
    assert(server.game.count == 128 && random_calls > 0);
    assert(gameplay_seed == 0x12345678); // Naming only advances local randomness.
    puts("Blank/whitespace names, unloaded UI tags, preserved custom names and 128 unique host-assigned names passed.");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="halo-player-names-") as directory:
    path = Path(directory)

    def run(name, functions):
        source = path / f"{name}.c"
        source.write_text(BOUNDARY + functions + CASES)
        output = path / name
        subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-multichar", "-fsanitize=address,undefined", str(source), "-o", str(output)], check=True)
        return subprocess.run([str(output)], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    fixed = run("fixed", FUNCTIONS)
    assert fixed.returncode == 0, fixed.stderr
    print(fixed.stdout.strip())
    original = "if (network_game_player_name_is_blank(player_name))"
    assert original in FUNCTIONS
    control = run("missing-fallback", FUNCTIONS.replace(original, "if (FALSE)"))
    assert control.returncode != 0 and "Assertion" in control.stderr, control.stderr
    print("Original unavailable-tag defect rejected by the regression.")
