"""Run the production server cleanup paths for accepted, unvalidated peers.

A replacement OPEN waits for the old descriptor to close. Accepted clients
already own that descriptor even before their join validates a game machine.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
SOURCE = (ROOT / "source/networking/network_server_manager.c").read_text()


def function(name):
    match = re.search(r"static boolean\s*" + name + r"\([^;]*?\)\s*\{", SOURCE)
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
typedef unsigned short word;
#define HALO_LINUX 1
#define TRUE 1
#define FALSE 0
#define NONE (-1)
#define MAXIMUM_NETWORK_MACHINE_COUNT 128
#define MAXIMUM_NETWORK_MESSAGE_SIZE 64
#define NETWORK_SERVER_MANAGER_FILE "server"
#define _connection_dont_timeout 0
#define match_assert(file, line, condition) assert(condition)
#define network_event(...) ((void)0)
#define network_machine_is_valid(machine) ((machine)->machine_index != NONE)
struct network_connection { boolean active, idle_ok; };
struct network_machine { int machine_index; };
struct network_game_server_client_machine {
    int machine_index; unsigned long flags;
    struct network_connection *connection;
};
struct network_game_server {
    struct { struct network_machine machines[128]; } game;
    struct network_game_server_client_machine client_machines[128];
};
static int closed_endpoints, removed_machines;
static boolean network_connection_active(struct network_connection *connection) {
    return connection->active;
}
static boolean network_connection_idle(struct network_connection *connection, int flags, void *result) {
    (void)flags; (void)result; return connection->idle_ok;
}
static boolean network_connection_connected(struct network_connection *connection) {
    (void)connection; return TRUE;
}
static boolean network_connection_read(struct network_connection *connection, word *message, word *size, void *address) {
    (void)connection; (void)message; (void)size; (void)address; return FALSE;
}
static boolean network_game_server_handle_client_message(struct network_game_server *server,
    struct network_game_server_client_machine *client, word *message, word size) {
    (void)server; (void)client; (void)message; (void)size; return TRUE;
}
static boolean network_game_server_remove_client_machine_from_game(struct network_game_server *server,
    struct network_game_server_client_machine *client) {
    (void)server;
    assert(client->connection);
    client->connection = NULL;
    client->machine_index = NONE;
    closed_endpoints++;
    return TRUE;
}
static boolean network_game_server_remove_machine_from_game(struct network_game_server *server,
    struct network_machine *machine) {
    if (!network_machine_is_valid(machine)) return FALSE;
    int index = machine->machine_index;
    removed_machines++;
    assert(network_game_server_remove_client_machine_from_game(server, &server->client_machines[index]));
    machine->machine_index = NONE;
    return TRUE;
}
static boolean network_game_server_drop_client_machine(struct network_game_server *server,
    struct network_game_server_client_machine *client) {
    if ((client->flags & 2) && network_machine_is_valid(&server->game.machines[client->machine_index]))
        return network_game_server_remove_machine_from_game(server, &server->game.machines[client->machine_index]);
    return network_game_server_remove_client_machine_from_game(server, client);
}
static void network_game_server_dump(struct network_game_server *server) { (void)server; }
'''

FUNCTIONS = function("network_game_server_remove_disconnected_client")

CASES = r'''
int main(void) {
    struct network_game_server server;
    struct network_connection connection;
    /* Cover EOF already observed and a read that fails during this idle. */
    for (int validated = 0; validated < 2; validated++) {
        for (int previously_closed = 0; previously_closed < 2; previously_closed++) {
            for (int reload = 0; reload < 48; reload++) {
                memset(&server, 0, sizeof(server));
                for (int i = 0; i < 128; i++) {
                    server.game.machines[i].machine_index = NONE;
                    server.client_machines[i].machine_index = NONE;
                }
                connection.active = !previously_closed;
                connection.idle_ok = FALSE;
                server.client_machines[1].machine_index = 1;
                server.client_machines[1].connection = &connection;
                if (validated) { server.game.machines[1].machine_index = 1; server.client_machines[1].flags = 2; }
                closed_endpoints = removed_machines = 0;
                assert(network_game_server_remove_disconnected_client(&server, &server.client_machines[1]));
                assert(closed_endpoints == 1);
                assert(removed_machines == validated);
                assert(server.client_machines[1].connection == NULL);
                assert(server.client_machines[1].machine_index == NONE);
            }
        }
    }
    puts("Accepted peer reload cleanup passed before and after join validation.");
}
'''

WEB_BOUNDARY = BOUNDARY.replace("struct network_game_server {", "struct network_game_server { int state;") + r'''
#define HALO_WEB 1
#define _network_game_server_state_ingame 1
#define _network_client_machine_validated_bit 1
#define TEST_FLAG(flags,bit) ((flags)&(1UL<<(bit)))
static struct { unsigned long epoch; } server_migration;
static unsigned long network_game_server_client_machine_addresses[128];
boolean web_match_migration_enabled(void) { return TRUE; }
static void network_game_server_migration_detach(struct network_game_server *server,
    struct network_game_server_client_machine *client) {
    assert(network_game_server_remove_client_machine_from_game(server, client));
}
'''

WEB_CASES = r'''
int main(void) {
    for (int epoch = 0; epoch < 2; epoch++) {
        for (int validated = 0; validated < 2; validated++) {
            for (int previously_closed = 0; previously_closed < 2; previously_closed++) {
                struct network_game_server server = {0};
                struct network_connection connection = {!previously_closed, FALSE};
                for (int i = 0; i < 128; i++) {
                    server.game.machines[i].machine_index = NONE;
                    server.client_machines[i].machine_index = NONE;
                }
                server.state = _network_game_server_state_ingame;
                server_migration.epoch = epoch;
                /* A provisional stream index collides with a retained roster
                slot until the original owner validates its reattach request. */
                server.game.machines[1].machine_index = 1;
                server.client_machines[1].machine_index = 1;
                server.client_machines[1].flags = validated ? 2 : 0;
                server.client_machines[1].connection = &connection;
                closed_endpoints = removed_machines = 0;
                assert(network_game_server_remove_disconnected_client(&server, &server.client_machines[1]));
                assert(closed_endpoints == 1 && removed_machines == 0);
                assert(server.client_machines[1].connection == NULL);
                assert(server.game.machines[1].machine_index == 1);
            }
        }
    }
    puts("Live-match pending reattach cleanup preserves roster at epochs zero and one.");
}
'''

with tempfile.TemporaryDirectory(prefix="halo-pending-rejoin-") as directory:
    path = Path(directory)

    def run(name, functions, boundary=BOUNDARY, cases=CASES):
        source = path / f"{name}.c"
        source.write_text(boundary + functions + cases)
        output = path / name
        subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-variable",
                        "-fsanitize=address,undefined", str(source), "-o", str(output)], check=True)
        return subprocess.run([str(output)], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    fixed = run("fixed", FUNCTIONS)
    assert fixed.returncode == 0, fixed.stderr
    print(fixed.stdout.strip())
    fallback = "return network_game_server_drop_client_machine(server, client);"
    assert fallback in FUNCTIONS
    control = run("missing-unvalidated-cleanup", FUNCTIONS.replace(fallback,
        "return network_game_server_remove_machine_from_game(server, &server->game.machines[client->machine_index]);"))
    assert control.returncode != 0, "Original accepted-peer leak must fail the regression"
    assert "Assertion" in control.stderr, control.stderr
    print("Original-defect control rejected: missing unvalidated endpoint cleanup")
    fixed = run("live-match", FUNCTIONS, WEB_BOUNDARY, WEB_CASES)
    assert fixed.returncode == 0, fixed.stderr
    print(fixed.stdout.strip())
    condition = "server_migration.epoch || (server->state == _network_game_server_state_ingame && web_match_migration_enabled())"
    assert condition in FUNCTIONS
    control = run("missing-epoch-zero-pending-cleanup", FUNCTIONS.replace(condition, "server_migration.epoch"), WEB_BOUNDARY, WEB_CASES)
    assert control.returncode != 0 and "Assertion" in control.stderr, control.stderr
    print("Original-defect control rejected: epoch-zero pending cleanup removes retained machine")
