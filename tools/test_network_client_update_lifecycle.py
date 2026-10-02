"""Run production gameplay handlers across reconnect/loading states.

Updates and membership deltas for the previous connection must not tear down a
distributed join or mutate its new settings. Active-game decoding and apply
failures follow the current distributed message handlers.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
SOURCE = (ROOT / "source/networking/network_client_message_handler.c").read_text()
KINDS = ("game_update", "add_player_ingame", "remove_player_ingame")


def function(name):
    match = re.search(r"static boolean " + name + r"\([^;]*?\)\s*\{", SOURCE)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        end += 1
    return SOURCE[match.start():end]


FUNCTION = "#ifdef HALO_LINUX\n" + function("network_game_client_ignores_early_ingame_messages") + "\n#endif\n"
FUNCTION += "\n".join(function("network_game_client_handle_message_server_" + kind) for kind in KINDS)

BOUNDARY = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef int boolean;
typedef unsigned short word;
#define TRUE 1
#define FALSE 0
#define NETWORK_GAME_MESSAGE_VERSION 2
#define network_event(...) ((void)0)
enum { _network_game_client_state_searching, _network_game_client_state_joining,
    _network_game_client_state_pregame, _network_game_client_state_ingame,
    _network_game_client_state_postgame };
enum { _message_server_game_update = 17, _message_server_add_player_ingame,
    _message_server_remove_player_ingame, _network_game_packet_class_ingame = 4 };
struct network_game_client { int state, oos; long tick; int updates, members; };
struct message_server_game_update { long tick; };
struct network_player { int id; };
struct message_server_remove_player_ingame { struct network_player player; long reason; };
struct transport_address { int matches; };
static int distributed, decodes, applications, fail_decode, fail_apply;
static boolean network_game_distributed(void) { return distributed; }
static boolean network_game_client_address_matches_server(struct network_game_client *client,
    struct transport_address *address) { (void)client; return address->matches; }
static short network_game_client_get_state(struct network_game_client *client, short *data) {
    assert(!data); return (short)client->state;
}
static boolean decode_network_game_message(void *decoded,
    word *bytes, short *size, short *type, short *version, int packet_class) {
    assert(bytes && *size == 4 && *type >= _message_server_game_update && *type <= _message_server_remove_player_ingame);
    assert(*version == NETWORK_GAME_MESSAGE_VERSION && packet_class == _network_game_packet_class_ingame);
    decodes++;
    if (*type == _message_server_game_update) ((struct message_server_game_update *)decoded)->tick = 777;
    else if (*type == _message_server_add_player_ingame) ((struct network_player *)decoded)->id = 11;
    else *(struct message_server_remove_player_ingame *)decoded = (struct message_server_remove_player_ingame){ {22}, 100 };
    return !fail_decode;
}
static boolean network_game_client_handle_game_update(struct network_game_client *client,
    struct message_server_game_update *update) {
    applications++;
    if (fail_apply) return FALSE;
    client->tick = update->tick; client->updates++; return TRUE;
}
static boolean network_game_client_add_player_to_game(struct network_game_client *client, struct network_player *player) {
    assert(player->id == 11); applications++;
    if (fail_apply) return FALSE;
    client->members++; return TRUE;
}
static boolean network_game_client_remove_player(struct network_game_client *client, struct network_player *player, long reason) {
    assert(player->id == 22 && reason == 100); applications++;
    if (fail_apply) return FALSE;
    client->members--; return TRUE;
}
static void network_game_client_game_out_of_sync(struct network_game_client *client) { client->oos++; }
'''

CASES = r'''
static struct transport_address host = { TRUE }, other = { FALSE };
static word packet[3];
typedef boolean (*handler)(struct network_game_client *, word *, short, struct transport_address *);
static handler handlers[] = {
    network_game_client_handle_message_server_game_update,
    network_game_client_handle_message_server_add_player_ingame,
    network_game_client_handle_message_server_remove_player_ingame,
};
static void reset(struct network_game_client *client, int state) {
    memset(client, 0, sizeof(*client)); client->state = state; client->tick = 10; client->members = 2;
    distributed = TRUE; decodes = applications = fail_decode = fail_apply = 0;
}
static boolean handle(struct network_game_client *client, int kind) {
    return handlers[kind](client, packet, sizeof(packet), &host);
}
int main(void) {
    struct network_game_client client;
    /* A queued update reaches the reloaded client before acceptance/map load.
       Ignore it without decoding, applying, advancing time or setting OOS. */
    for (int kind = 0; kind < 3; kind++)
    for (int state = _network_game_client_state_searching; state <= _network_game_client_state_postgame; state++) {
        if (state == _network_game_client_state_ingame) continue;
        reset(&client, state);
#ifdef HALO_LINUX
        assert(handle(&client, kind)); assert(!client.oos);
#else
        assert(!handle(&client, kind)); assert(client.oos == 1);
#endif
        assert(!decodes && !applications && client.tick == 10 && !client.updates && client.members == 2 && client.state == state);
    }
#ifdef HALO_LINUX
    reset(&client, _network_game_client_state_joining);
    for (int i = 0; i < 30; i++) for (int kind = 0; kind < 3; kind++) assert(handle(&client, kind));
    client.state = _network_game_client_state_pregame;
    for (int i = 0; i < 30; i++) for (int kind = 0; kind < 3; kind++) assert(handle(&client, kind));
    assert(!decodes && !applications && !client.oos && client.tick == 10 && client.members == 2);
    client.state = _network_game_client_state_ingame;
    for (int kind = 0; kind < 3; kind++) assert(handle(&client, kind));
    assert(decodes == 3 && applications == 3 && !client.oos && client.tick == 777 && client.updates == 1 && client.members == 2);
#endif
    for (int kind = 0; kind < 3; kind++) {
    reset(&client, _network_game_client_state_ingame);
    assert(handle(&client, kind)); assert(decodes == 1 && applications == 1 && !client.oos);
    assert(client.members == (kind == 1 ? 3 : kind == 2 ? 1 : 2));
    reset(&client, _network_game_client_state_ingame); fail_decode = TRUE;
    assert(!handle(&client, kind)); assert(decodes == 1 && !applications && !client.oos && client.tick == 10 && client.members == 2);
    reset(&client, _network_game_client_state_ingame); fail_apply = TRUE;
    assert(handle(&client, kind) == (kind != 0)); assert(decodes == 1 && applications == 1 && !client.oos && client.tick == 10 && client.members == 2);
    reset(&client, _network_game_client_state_ingame);
    assert(handlers[kind](&client, packet, sizeof(packet), &other));
    assert(!decodes && !applications && !client.oos && client.tick == 10);
    /* Lockstep retains its strict game-state handling. */
    reset(&client, _network_game_client_state_joining); distributed = FALSE;
    assert(handle(&client, kind)); assert(!client.oos && !decodes && !applications);
    }
    puts("Distributed reconnect/load updates and membership, active-game errors, source address and lockstep behavior passed.");
}
'''

with tempfile.TemporaryDirectory(prefix="halo-update-lifecycle-") as directory:
    path = Path(directory)

    def run(name, function, native):
        source = path / f"{name}.c"
        source.write_text(BOUNDARY + function + CASES)
        output = path / name
        subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-function", "-fsanitize=address,undefined",
                        *(["-DHALO_LINUX=1"] if native else []), str(source), "-o", str(output)], check=True)
        return subprocess.run([str(output)], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    for native in (True,):
        result = run("native" if native else "xbox-contract", FUNCTION, native)
        assert result.returncode == 0, result.stderr
        print(result.stdout.strip())
    for kind in KINDS:
        original = f'network_event("ignoring a message_server_{kind} message before loading the game");\n\t\t\tresult = TRUE;'
        assert original in FUNCTION
        control = run("original-" + kind, FUNCTION.replace(original, "((void)0);"), True)
        assert control.returncode != 0 and "Assertion" in control.stderr, control.stderr
        print(f"Original reconnect out-of-sync behavior rejected: {kind}")
