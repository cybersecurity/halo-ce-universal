/* Browser host handoff changes transport ownership, never creates a match. */
#ifndef __NETWORK_MIGRATION_H
#define __NETWORK_MIGRATION_H

#include "cseries.h"
#include "bungie_net/common/message_header.h"

struct network_game_client;
struct network_game_server;
struct network_game_server_client_machine;
struct transport_address;

/* Separate from distributed gameplay message kinds. This handshake is only
accepted on a newly established reliable transport, with its original owner. */
enum { NETWORK_MIGRATION_MESSAGE = 0xF0, NETWORK_MIGRATION_VERSION = 1,
	NETWORK_MIGRATION_ATTACH = 1, NETWORK_MIGRATION_ACK = 2 };
struct network_migration_message
{
	message_header header;
	byte type, version;
	unsigned long epoch, session_seed;
	short machine_index, host_machine_index, departed_machine_index, kind;
	long game_tick;
	byte machine_present[(HALO_PORT_MAXIMUM_NETWORK_MACHINES + 7) / 8];
};

/* The in-game player stream does not carry the complete machine roster.
Checkpoint its authoritative membership, including reused and absent slots. */
struct network_migration_machine
{
	byte name[0x40];
	char machine_index;
	byte pad[3];
};
struct network_migration_membership
{
	long machine_count;
	struct network_migration_machine machines[HALO_PORT_MAXIMUM_NETWORK_MACHINES];
};

#ifdef HALO_WEB
unsigned long network_game_migration_epoch(void);
boolean create_global_network_game_server_from_migration(unsigned long epoch);
boolean network_game_client_begin_migration(struct network_game_client *client,
	unsigned long target, unsigned long epoch);
boolean network_game_client_migration_ready(struct network_game_client *client);
boolean network_game_client_migration_waiting(struct network_game_client *client);
boolean network_game_server_migration_ready(struct network_game_server *server);
boolean network_game_server_recover_match(struct network_game_server *server, unsigned long epoch);
void network_game_server_migration_finish(struct network_game_server *server);
struct network_game_server *network_game_server_adopt_match(struct network_game_client *client,
	unsigned long epoch);
boolean network_game_client_handle_migration(struct network_game_client *client,
	void const *message, word size, struct transport_address *address);
boolean network_game_server_handle_migration(struct network_game_server *server,
	struct network_game_server_client_machine *machine, void const *message, word size);
void network_game_server_migration_routes(unsigned long *addresses, short count);
boolean network_game_server_migration_machines(void *buffer, long size);
boolean network_game_client_migration_validate_machines(void const *buffer, long size);
boolean network_game_client_migration_set_machines(void const *buffer, long size);
void network_game_client_migration_routes(unsigned long const *addresses, short count);
unsigned long network_game_client_migration_owner_address(short machine_index);
short network_game_client_migration_host_machine(struct network_game_client *client);
void network_game_client_migration_set_host_machine(short machine_index);
unsigned long web_quick_play_initial_epoch(void);
void network_game_client_migration_remove_machine(struct network_game_client *client, short machine_index);
void update_queues_migrate(void);
void network_game_server_release_migration_transport(struct network_game_server *server);
void network_game_demote_migration_host(void);
boolean web_match_migration_enabled(void);
boolean web_match_migration_lost(void);
unsigned long web_quick_play_address(void);
#endif
#endif
