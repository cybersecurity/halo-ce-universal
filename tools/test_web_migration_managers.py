"""Exercise production host-adoption and reattach code with bounded transports.

This runs the actual C manager functions, isolating renderer/network IO. It
checks identity/generation admission, original roster IDs, retained game clock
and objects, cohort resume, lost final ACK replay, and a second migration.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
CLIENT = "source/networking/network_client_manager.c"
SERVER = "source/networking/network_server_manager.c"
GLOBALS = "source/networking/network_game_globals.c"
HEADERS = "source/bungie_net/common/message_header.c"
GAME = "source/networking/network_game_manager.c"
PLAYERS = "source/game/players.c"


def function(path, name):
    source = (ROOT / path).read_text()
    match = re.search(r"^(?:static )?(?:boolean|void|short|unsigned long|struct network_game_server \*)\s*" + name + r"\([^;]*?\)\s*\{", source, re.M)
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
#define HALO_WEB 1
#define HALO_LINUX 1
typedef int boolean;
typedef unsigned char byte;
typedef unsigned short word, message_header;
#define TRUE 1
#define FALSE 0
#define NONE (-1)
#define MAXIMUM_NETWORK_MACHINE_COUNT 128
#define MAXIMUM_NUMBER_OF_PLAYERS 128
#define NETWORK_GAME_PLAYER_SLOTS 128
#define MAXIMUM_LOCAL_PLAYERS 4
#define VALID_INDEX(index,limit) ((index)>=0 && (index)<(limit))
#define HALO_PORT_MAXIMUM_NETWORK_MACHINES 128
#define FLAG(bit) (1UL << (bit))
#define TEST_FLAG(flags,bit) ((flags)&FLAG(bit))
#define SET_FLAG(flags,bit,value) ((flags) = (value) ? (flags)|FLAG(bit) : (flags)&~FLAG(bit))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define GET_MESSAGE_FLAGS(h) ((h)&3)
#define GET_MESSAGE_TYPE(h) (((h)>>2)&3)
#define GET_MESSAGE_SIZE(h) ((h)>>4)
#define MAXIMUM_MESSAGE_SIZE 0xfff
#define NUMBER_OF_MESSAGE_TYPES 4
#define MESSAGE_FLAG_BITS_MASK 3
enum message_header_byte_order { _byte_order_host, _byte_order_network };
void build_message_header(word *msg, word length, byte type, byte flags);
void byte_swap_message_header(word *header, enum message_header_byte_order byte_order);
#define csmemset memset
#define csmemcpy memcpy
#define network_event(...) ((void)0)
#define error(...) ((void)0)
#define match_assert(file,line,condition) assert(condition)
#define network_machine_is_valid(m) ((m)->machine_index>=0)
#define network_player_is_valid(p) ((p)->machine_index>=0 && (p)->machine_index<128)
#define IPV4_LOOPBACK_ADDRESS 0x7f000001UL
#define NETWORK_GAME_SERVER_PORT 2302
#define NETWORK_GAME_CLIENT_PORT 2303
#define IPV4_ADDRESS_LENGTH 4
#define _network_connection_type_client 1
#define _connection_create_server_bit 0
#define _network_game_server_state_pregame 0
#define _network_game_server_state_ingame 1
#define _network_game_server_state_postgame 2
#define MAXIMUM_WAITING_CONNECTIONS_PER_ADDRESS 2
#define _network_game_client_state_ingame 3
#define _network_game_server_game_valid_bit 1
#define _network_game_server_game_open_bit 0
#define _network_game_client_error_none 0
#define _network_client_machine_connected_bit 0
#define _network_client_machine_validated_bit 1
#define _network_client_machine_level_loaded_bit 2
#define _network_client_machine_precached_bit 3
struct transport_address { union { unsigned long long_words[1], ipv4_address; } address; word port; short address_length; };
struct network_connection { unsigned long address; int connected, active; boolean allow_clients; };
struct network_machine { byte name[0x40]; char machine_index; byte pad[3]; };
struct network_player { short machine_index, player_list_index, controller_index, team_index; };
struct player_datum { unsigned short identifier; long quit_out_of_game_time; int score; float position[3]; };
struct network_game { unsigned long random_seed; short machine_count, player_count, maximum_players;
 struct network_machine machines[128]; struct network_player players[128]; };
struct network_game_client { word machine_index; struct network_game game;
 struct network_connection *connection; void *connect_process; short state,error;
 boolean out_of_sync,connection_silent; unsigned long next_update_number,last_update_time; };
struct network_game_server_client_machine { struct network_connection *connection;
 unsigned long last_received_update_sequence_number, last_heard_time; short machine_index; word flags; };
struct network_game_server { struct network_connection *connection; word state,flags;
 struct network_game game; struct network_game_server_client_machine client_machines[128];
 boolean sent_start_game_message; long next_update_number,time_of_last_keep_alive; };
static struct network_game_server network_game_server_memory_do_not_use_directly;
static boolean network_game_server_memory_do_not_use_directly_in_use;
static unsigned long network_game_server_client_machine_addresses[128];
static struct { boolean reconnecting,acknowledged; unsigned long epoch,target,retry_at,attach_at;
 short host_machine; unsigned long owner_addresses[128]; } client_migration;
static struct { boolean adopting; unsigned long epoch; short host_machine,departed_machine;
 unsigned long owner_addresses[128], disconnected_at[128]; } server_migration;
static struct network_game_client local_client;
static struct network_game_server *global_server;
static struct { boolean accept_remote_connections, client_started; } bss_004566dc;
static struct network_game_client *global_network_game_client;
#define global_network_game_server global_server
#define _game_connection_network_server 2
static int promotions;
static struct network_connection transports[260];
static struct player_datum datums[128];
static long machine_to_player_table[128][4];
static long tick=900;
static unsigned long now=10000;
static int next_transport, writes, deletes, resets, queue_rebases, demotions, admissions;
static boolean idle_success=TRUE;
static boolean fail_connection_allocation;
'''
HEADER = (ROOT / "source/networking/network_migration.h").read_text()
WIRE = HEADER[HEADER.index("enum { NETWORK_MIGRATION_MESSAGE"):HEADER.index("#ifdef HALO_WEB")]
STUBS = r'''
static struct network_migration_message sent[260];
static boolean network_connection_active(struct network_connection *c) { return c && c->active; }
static boolean network_connection_connected(struct network_connection *c) { return c && c->connected; }
static boolean network_connection_idle(struct network_connection *c, long timeout, void *unused) {
 (void)c; (void)unused; assert(timeout==15000); return idle_success; }
static void network_connection_connect(struct network_connection *c, struct transport_address *a, void *unused) {
 (void)unused; c->address=a->address.long_words[0]; c->connected=c->active=TRUE; }
static boolean network_game_client_process_incoming_messages(struct network_game_client *c) { (void)c; return TRUE; }
static long game_time_get(void) { return tick; }
static void game_time_set_distributed(long t) { tick=t; }
static unsigned long system_milliseconds(void) { return now; }
static unsigned long web_quick_play_address(void) { return 0x64563811UL; }
static boolean network_distributed_migration_ready(void) { return TRUE; }
static boolean web_match_migration_enabled(void) { return TRUE; }
static boolean network_distributed_migration_promote(void) { promotions++; return TRUE; }
static void game_connection_set(short connection) { assert(connection==_game_connection_network_server); }
static void network_game_server_send_player_quit_messages_ingame(struct network_game_server *s,
 struct network_game_server_client_machine *m) { (void)s; datums[m->machine_index].quit_out_of_game_time=tick+1; }
static short network_game_client_get_state(struct network_game_client *c, void *unused) { (void)unused; return c->state; }
static short network_game_client_get_machine_index(struct network_game_client *c) { return c->machine_index; }
static struct network_game *network_game_client_get_game(struct network_game_client *c) { return &c->game; }
static struct network_game_client *global_network_game_client_get(void) { return &local_client; }
static struct network_game_server *global_network_game_server_get(void) { return global_server; }
static void network_game_demote_migration_host(void) { demotions++; global_server=NULL; }
static struct network_connection *network_connection_new(unsigned long flags, word port) {
 (void)flags; (void)port; return fail_connection_allocation?NULL:&transports[next_transport++]; }
static void network_connection_delete(struct network_connection *c) { (void)c; deletes++; }
static boolean network_server_close_client_connection(struct network_connection *s, struct network_connection *c) {
 (void)s; c->connected=FALSE; deletes++; return TRUE; }
static void cancel_connect_process(void *p) { (void)p; }
static void network_connection_set_connection_rejection_procedure(struct network_connection *c, void (*p)(void)) { (void)c; (void)p; }
static void network_game_server_reject_connection_game_is_full(void) { }
static void network_server_allow_client_connections(struct network_connection *c, boolean allow) { c->allow_clients=allow; }
static boolean network_connection_server_accept_client_connection(struct network_connection *s, struct network_connection *c) {
 assert(s->allow_clients); c->connected=TRUE; admissions++; return TRUE; }
static void network_game_invalidate_machine(struct network_game *g, word machine) {
 (void)g; (void)machine; assert(!"migration must not invalidate a preserved roster slot"); }
static void network_connection_get_address(struct network_connection *c, struct transport_address *a, void *unused) {
 (void)unused; a->address.long_words[0]=c->address; }
static boolean network_connection_write(struct network_connection *c, void *m, word size, void *a, boolean reliable) {
 (void)c; (void)a; assert(reliable && size==sizeof(struct network_migration_message));
 /* The production write boundary swaps its caller's header and leaves
 it swapped. Decode the copied wire packet as the receiving stream does. */
 word *header=m; assert(GET_MESSAGE_SIZE(*header)==size);
 byte_swap_message_header(header,_byte_order_network);
 sent[writes]=*(struct network_migration_message *)m;
 byte_swap_message_header(&sent[writes].header,_byte_order_host);
 assert(GET_MESSAGE_SIZE(sent[writes].header)==size && GET_MESSAGE_TYPE(sent[writes].header)==2 && !GET_MESSAGE_FLAGS(sent[writes].header));
 writes++; return TRUE; }
static boolean network_game_client_address_matches_server(struct network_game_client *c, struct transport_address *a) {
 (void)c; return a->address.long_words[0]==client_migration.target; }
static void network_distributed_migration_reset_transport(void) { resets++; }
static void update_queues_migrate(void) { queue_rebases++; }
static long unstrip_player_index(long p) { return p>=0 && p<128 && datums[p].identifier?p:NONE; }
static struct player_datum *player_try_and_get(long p) { return p>=0 && p<128?&datums[p]:NULL; }
static long *machine_get_player_list(long machine) { return machine_to_player_table[machine]; }
static boolean network_game_player_slot_held(long slot) { return datums[slot].identifier!=0; }
static boolean network_game_spawn_player(struct network_player *p) {
 long slot=p->player_list_index; assert(slot>=0 && slot<128 && !datums[slot].identifier);
 datums[slot].identifier=100+slot; datums[slot].quit_out_of_game_time=NONE;
 for(int i=0;i<4;i++) if(machine_to_player_table[p->machine_index][i]==NONE) {
 machine_to_player_table[p->machine_index][i]=slot; return TRUE; }
 return FALSE; }
static void game_engine_player_added(long player) { (void)player; }
static void local_player_set_player_index(long controller,long player) { (void)controller; (void)player; }
static void update_client_add_player(long player) { (void)player; }
static void update_server_add_player(long player) { (void)player; }
static boolean network_game_remove_machine(struct network_game *g, struct network_machine *m) {
 short id=m->machine_index; if(id<0) return FALSE;
 for(int i=0;i<128;i++) if(g->players[i].machine_index==id) { g->players[i].machine_index=NONE; g->player_count--; }
 m->machine_index=NONE; g->machine_count--; return TRUE; }
static boolean network_game_server_remove_machine_from_game(struct network_game_server *s, struct network_machine *m) {
 return network_game_remove_machine(&s->game,m); }
static boolean network_game_server_remove_client_machine_from_game(struct network_game_server *s,
 struct network_game_server_client_machine *m) {
 (void)s; m->connection=NULL; m->machine_index=NONE; m->flags=0; return TRUE; }
static boolean network_game_server_client_machine_is_joined_to_game(
 struct network_game_server *s, struct network_game_server_client_machine *m)
{ (void)s; return TEST_FLAG(m->flags, _network_client_machine_validated_bit); }
static boolean network_game_server_drop_client_machine(struct network_game_server *s,
 struct network_game_server_client_machine *m) {
 if (network_game_server_client_machine_is_joined_to_game(s,m) &&
     network_game_server_remove_machine_from_game(s,&s->game.machines[m->machine_index])) return TRUE;
 return network_game_server_remove_client_machine_from_game(s,m); }
'''
FUNCTIONS = "\n".join((
    function(HEADERS, "build_message_header"),
    function(HEADERS, "byte_swap_message_header"),
    function(GLOBALS, "network_game_accept_remote_connections"),
    function(GLOBALS, "network_game_should_accept_remote_connections"),
    function(PLAYERS, "network_player_remove_from_machine"),
    function(GAME, "network_game_add_player"),
    function(CLIENT, "network_game_client_sync_added_machine"),
    function(CLIENT, "network_game_client_add_player_to_game"),
    function(CLIENT, "network_game_migration_epoch"),
    function(CLIENT, "network_game_client_migration_routes"),
    function(CLIENT, "network_game_client_migration_validate_machines"),
    function(CLIENT, "network_game_client_migration_set_machines"),
    function(CLIENT, "network_game_client_migration_owner_address"),
    function(CLIENT, "network_game_client_migration_host_machine"),
    function(CLIENT, "network_game_client_migration_remove_machine"),
    function(CLIENT, "network_game_client_begin_migration"),
    function(CLIENT, "network_game_client_idle_migration"),
    function(CLIENT, "network_game_client_migration_ready"),
    function(CLIENT, "network_game_client_handle_migration"),
    function(SERVER, "network_game_server_open_game"),
    function(SERVER, "network_game_server_game_is_open"),
    function(SERVER, "network_game_server_add_new_client"),
    function(SERVER, "network_game_server_adopt_match"),
    function(SERVER, "network_game_server_migration_machines"),
    function(SERVER, "network_game_server_migration_detach"),
    function(SERVER, "network_game_server_migration_expire_disconnected"),
    function(SERVER, "network_game_server_remove_disconnected_client"),
    function(SERVER, "network_game_server_migration_routes"),
    function(SERVER, "network_game_server_recover_match"),
    function(SERVER, "network_game_server_migration_acknowledge"),
    function(SERVER, "network_game_server_handle_migration"),
    function(SERVER, "network_game_server_migration_ready"),
    function(SERVER, "network_game_server_migration_finish"),
    function(GLOBALS, "create_global_network_game_server_from_migration"),
))
TESTS = r'''
static void setup(void) {
 memset(&local_client,0,sizeof(local_client)); memset(&client_migration,0,sizeof(client_migration));
 memset(&server_migration,0,sizeof(server_migration)); memset(datums,0,sizeof(datums));
 memset(machine_to_player_table,0xff,sizeof(machine_to_player_table));
 network_game_server_memory_do_not_use_directly_in_use=FALSE; global_server=NULL; writes=0; tick=900;
 global_network_game_client=&local_client; promotions=0; demotions=0;
 bss_004566dc.accept_remote_connections=FALSE; admissions=0;
 local_client.machine_index=1; local_client.state=_network_game_client_state_ingame;
 local_client.game.random_seed=42; local_client.game.machine_count=3; local_client.game.player_count=3;
 local_client.game.maximum_players=128;
 for(int i=0;i<128;i++) { local_client.game.machines[i].machine_index=NONE; local_client.game.players[i].machine_index=NONE;
 local_client.game.players[i].player_list_index=NONE; }
 for(int i=0;i<3;i++) { local_client.game.machines[i].machine_index=i; local_client.game.players[i].machine_index=i;
 local_client.game.players[i].player_list_index=i; datums[i].identifier=100+i; datums[i].quit_out_of_game_time=NONE;
 datums[i].score=17+i; datums[i].position[0]=100+i; client_migration.owner_addresses[i]=0x64563810UL+i; }
 for(int i=0;i<3;i++) machine_to_player_table[i][0]=i;
 local_client.connection=&transports[next_transport++];
}
static struct network_migration_message attach(short machine) {
 struct network_migration_message m={0}; m.type=NETWORK_MIGRATION_MESSAGE; m.version=NETWORK_MIGRATION_VERSION;
 m.epoch=1; m.session_seed=42; m.machine_index=machine; m.kind=NETWORK_MIGRATION_ATTACH;
 build_message_header(&m.header,sizeof(m),2,0); return m;
}
static void preserved(void) {
 assert(tick==900 && local_client.state==_network_game_client_state_ingame);
 assert(datums[1].identifier==101 && datums[2].identifier==102 && datums[1].score==18 && datums[2].score==19);
 assert(datums[1].position[0]==101 && datums[2].position[0]==102);
 assert(local_client.game.players[1].player_list_index==1 && local_client.game.players[2].player_list_index==2);
}
int main(void) {
 /* Repair a single client at epoch zero, replacing its half-open stream
 only after matching its recorded owner. Other players and the host continue. */
 setup();
 {
  struct network_game_server original={0}; original.game=local_client.game;
  original.state=_network_game_server_state_ingame;
  original.connection=&transports[next_transport++]; global_server=&original;
  for(int i=0;i<128;i++) original.client_machines[i].machine_index=NONE;
  for(int i=0;i<3;i++) {
   struct network_game_server_client_machine *c=&original.client_machines[i];
   c->machine_index=i; c->flags=FLAG(_network_client_machine_validated_bit);
   c->connection=&transports[next_transport++]; c->connection->address=0x64563810UL+i;
  }
  original.client_machines[1].connection->address=IPV4_LOOPBACK_ADDRESS;
  /* Exercise stream admission after the old endpoint detached: its free
  transport index still names a valid retained machine in the roster. */
  assert(network_game_server_remove_disconnected_client(&original,&original.client_machines[2]));
  network_game_server_open_game(&original); network_game_accept_remote_connections(TRUE);
  struct network_connection *repair_stream=&transports[next_transport++]; repair_stream->address=0x64563899UL;
  assert(network_game_server_add_new_client(&original,repair_stream));
  struct network_game_server_client_machine *repaired=&original.client_machines[2];
  assert(original.game.machine_count==3 && original.game.machines[2].machine_index==2);
  struct network_migration_message repair=attach(2); repair.epoch=0;
  network_game_server_handle_migration(&original,repaired,&repair,sizeof(repair));
  assert(!writes && !TEST_FLAG(repaired->flags,_network_client_machine_validated_bit));
  repaired->connection->address=0x64563812UL;
  network_game_server_handle_migration(&original,repaired,&repair,sizeof(repair));
  assert(writes==3 && repaired->connection==repair_stream && repaired->machine_index==2);
  assert(original.game.machine_count==3 && original.game.player_count==3 && tick==900);
  assert(sent[2].epoch==0 && sent[2].machine_index==2 && sent[2].host_machine_index==1); preserved();
  struct network_connection *half_open=repaired->connection;
  struct network_connection *retry=&transports[next_transport++]; retry->address=0x64563812UL;
  assert(network_game_server_add_new_client(&original,retry));
  repaired=&original.client_machines[3]; writes=0;
  network_game_server_handle_migration(&original,repaired,&repair,sizeof(repair));
  assert(writes==3 && !half_open->connected && !original.client_machines[2].connection && repaired->machine_index==2);
  assert(original.game.machine_count==3 && original.game.player_count==3); preserved();
  global_server=NULL; local_client.machine_index=2;
  assert(network_game_client_begin_migration(&local_client,0x64563811UL,0));
  struct transport_address owner={0}; owner.address.long_words[0]=0x64563811UL;
  network_game_client_handle_migration(&local_client,&sent[2],sizeof(sent[2]),&owner);
  assert(network_game_client_migration_ready(&local_client) && !demotions && network_game_migration_epoch()==0);
  assert(datums[2].identifier==102 && datums[2].score==19 && machine_to_player_table[2][0]==2); preserved();
 }
 /* The original host is still playing when a client reports an outage.
 Re-electing it must renew connections without promoting a stale snapshot,
 removing its own player, or creating another game. */
 setup();
 struct network_game_server existing={0}; existing.game=local_client.game;
 existing.state=_network_game_server_state_ingame; existing.sent_start_game_message=TRUE;
 existing.connection=&transports[next_transport++]; global_server=&existing;
 for(int i=0;i<128;i++) existing.client_machines[i].machine_index=NONE;
 for(int i=0;i<3;i++) {
  struct network_game_server_client_machine *c=&existing.client_machines[i];
  c->machine_index=i; c->flags=FLAG(_network_client_machine_validated_bit);
  c->connection=&transports[next_transport++]; c->connection->address=0x64563810UL+i;
 }
 existing.client_machines[1].connection->address=IPV4_LOOPBACK_ADDRESS;
 /* RTC can drop before the original host observes the higher epoch. Its
 validated players must survive this epoch-zero disconnect path too. */
 assert(network_game_server_remove_disconnected_client(&existing,&existing.client_machines[0]));
 assert(network_game_server_remove_disconnected_client(&existing,&existing.client_machines[2]));
 assert(existing.game.machine_count==3 && datums[0].quit_out_of_game_time==NONE && datums[2].quit_out_of_game_time==NONE);
 assert(server_migration.owner_addresses[2]==0x64563812UL);
 assert(create_global_network_game_server_from_migration(1));
 assert(global_server==&existing && !promotions && tick==900 && existing.game.random_seed==42);
 assert(existing.game.machine_count==3 && existing.game.player_count==3 && datums[0].quit_out_of_game_time==NONE);
 assert(server_migration.host_machine==1 && server_migration.departed_machine==NONE && server_migration.adopting);
 assert(server_migration.owner_addresses[2]==0x64563812UL && existing.next_update_number==900);
 assert(!create_global_network_game_server_from_migration(1)); preserved();
 fail_connection_allocation=TRUE;
 assert(!create_global_network_game_server_from_migration(2) && !global_server);
 assert(!promotions && tick==900 && datums[0].quit_out_of_game_time==NONE); preserved();
 fail_connection_allocation=FALSE;
 setup();
 struct network_connection *remote=&transports[next_transport++]; remote->address=0x64563812UL;
 struct network_game_server closed_to_remotes={0}; closed_to_remotes.connection=&transports[next_transport++];
 for(int i=0;i<128;i++) closed_to_remotes.client_machines[i].machine_index=NONE;
 network_game_server_open_game(&closed_to_remotes);
 assert(!network_game_should_accept_remote_connections());
 assert(!network_game_server_add_new_client(&closed_to_remotes,remote) && admissions==0);
 struct network_game_server *s=network_game_server_adopt_match(&local_client,1); assert(s);
 assert(network_game_should_accept_remote_connections() && s->connection->allow_clients);
 assert(s->state==_network_game_server_state_ingame && s->game.machine_count==3 && s->game.player_count==3);
 assert(s->game.machines[0].machine_index==0 && s->game.machines[1].machine_index==1 && s->game.machines[2].machine_index==2);
 assert(s->next_update_number==900 && s->sent_start_game_message && datums[0].quit_out_of_game_time==NONE); preserved();
 assert(network_game_client_begin_migration(&local_client,IPV4_LOOPBACK_ADDRESS,1)); preserved();
 assert(!network_game_client_migration_ready(&local_client) && network_game_migration_epoch()==1);
 struct network_game_server_client_machine *a=&s->client_machines[0], *b=&s->client_machines[1];
 struct network_connection *local=&transports[next_transport++]; local->address=IPV4_LOOPBACK_ADDRESS;
 /* Exercise the production remote-admission gate, which menu hosting
 enables separately. Promotion must enable it for the healthy cohort. */
 assert(network_game_server_add_new_client(s,local) && a->connection==local && admissions==1);
 assert(network_game_server_add_new_client(s,remote) && b->connection==remote && admissions==2);
 assert(s->game.machine_count==3 && s->game.machines[1].machine_index==1 && s->game.machines[2].machine_index==2);
 struct network_migration_message m=attach(1), bad=m;
 bad.epoch=0; assert(network_game_server_handle_migration(s,a,&bad,sizeof(bad))); assert(!TEST_FLAG(a->flags,_network_client_machine_validated_bit) && !writes);
 bad=m; bad.header|=1; network_game_server_handle_migration(s,a,&bad,sizeof(bad)); assert(!TEST_FLAG(a->flags,_network_client_machine_validated_bit) && !writes);
 bad=m; bad.session_seed=43; network_game_server_handle_migration(s,a,&bad,sizeof(bad)); assert(!TEST_FLAG(a->flags,_network_client_machine_validated_bit) && !writes);
 bad=m; bad.machine_index=2; network_game_server_handle_migration(s,a,&bad,sizeof(bad)); assert(!TEST_FLAG(a->flags,_network_client_machine_validated_bit) && !writes);
 bad=m; bad.machine_index=0; network_game_server_handle_migration(s,a,&bad,sizeof(bad)); assert(!TEST_FLAG(a->flags,_network_client_machine_validated_bit) && !writes);
 network_game_server_handle_migration(s,a,&m,sizeof(m)); assert(a->machine_index==1 && !writes);
 assert(!network_game_server_migration_ready(s) && !network_game_client_migration_ready(&local_client));
 m=attach(2); bad=m; bad.epoch=2; network_game_server_handle_migration(s,b,&bad,sizeof(bad)); assert(!TEST_FLAG(b->flags,_network_client_machine_validated_bit));
 network_game_server_handle_migration(s,b,&m,sizeof(m)); assert(b->machine_index==2 && !writes);
 assert(network_game_server_migration_ready(s)); preserved();
 network_game_server_migration_finish(s); assert(writes==2 && sent[0].machine_index==1 && sent[1].machine_index==2);
 assert(sent[0].game_tick==900 && sent[0].host_machine_index==1 && sent[0].machine_present[0]==7);
 struct transport_address host={0}; host.address.long_words[0]=IPV4_LOOPBACK_ADDRESS;
 bad=sent[0]; bad.epoch=0; network_game_client_handle_migration(&local_client,&bad,sizeof(bad),&host);
 assert(!network_game_client_migration_ready(&local_client));
 host.address.long_words[0]=0x64563899UL; network_game_client_handle_migration(&local_client,&sent[0],sizeof(sent[0]),&host);
 assert(!network_game_client_migration_ready(&local_client));
 host.address.long_words[0]=IPV4_LOOPBACK_ADDRESS; network_game_client_handle_migration(&local_client,&sent[0],sizeof(sent[0]),&host);
 assert(network_game_client_migration_ready(&local_client) && client_migration.host_machine==1); preserved();
 /* Replaying the attach after finish must reproduce the final ACK. */
 m=attach(1); network_game_server_handle_migration(s,a,&m,sizeof(m)); assert(writes==4);
 /* A lost transport keeps the same machine/player identity for a retry. */
 network_game_server_migration_detach(s,b); assert(s->game.machines[2].machine_index==2 && server_migration.disconnected_at[2]);
 remote=&transports[next_transport++]; remote->address=0x64563812UL;
 assert(network_game_server_add_new_client(s,remote) && b->connection==remote && admissions==3);
 m=attach(2); network_game_server_handle_migration(s,b,&m,sizeof(m));
 assert(writes==6 && b->machine_index==2 && !server_migration.disconnected_at[2]); preserved();
 /* A second epoch demotes an obsolete listener and rejects old ACKs. */
 global_server=s; assert(network_game_client_begin_migration(&local_client,0x64563812UL,2));
 assert(demotions==1 && global_server==NULL && !network_game_client_migration_ready(&local_client));
 host.address.long_words[0]=0x64563812UL; network_game_client_handle_migration(&local_client,&sent[0],sizeof(sent[0]),&host);
 assert(!network_game_client_migration_ready(&local_client));
 bad=sent[0]; bad.epoch=2; bad.host_machine_index=2; network_game_client_handle_migration(&local_client,&bad,sizeof(bad),&host);
 assert(network_game_client_migration_ready(&local_client) && client_migration.host_machine==2); preserved();
 assert(!network_game_client_begin_migration(&local_client,IPV4_LOOPBACK_ADDRESS,1));
 assert(network_game_client_migration_ready(&local_client) && network_game_migration_epoch()==2);
 /* An active/connected transport can still time out. It must be retired,
 then retried on the same epoch without replacing player or world state. */
 setup(); assert(network_game_client_begin_migration(&local_client,0x64563812UL,1));
 network_game_client_idle_migration(&local_client); assert(local_client.connection && local_client.connection->connected);
 idle_success=FALSE; now+=15001; network_game_client_idle_migration(&local_client);
 assert(!local_client.connection); preserved();
 idle_success=TRUE; now+=1000; network_game_client_idle_migration(&local_client);
 assert(local_client.connection && local_client.connection->connected && local_client.connection->address==0x64563812UL);
 assert(client_migration.epoch==1 && client_migration.reconnecting && !client_migration.acknowledged); preserved();
 /* A delayed survivor retains its identity after the 12s cohort release,
 and still reattaches after the former 45s client deadline. */
 setup(); s=network_game_server_adopt_match(&local_client,1); assert(s);
 a=&s->client_machines[0]; local=&transports[next_transport++]; local->address=IPV4_LOOPBACK_ADDRESS;
 assert(network_game_server_add_new_client(s,local) && a->connection==local);
 m=attach(1); network_game_server_handle_migration(s,a,&m,sizeof(m)); assert(!network_game_server_migration_ready(s));
 network_game_server_migration_finish(s); assert(writes==1 && sent[0].machine_present[0]==7);
 unsigned long routes[128]; global_server=s; network_game_server_migration_routes(routes,128);
 assert(routes[2]==0x64563812UL);
 now+=60000; network_game_server_migration_expire_disconnected(s);
 assert(s->game.machine_count==3 && local_client.game.machine_count==3 && datums[2].quit_out_of_game_time==NONE);
 remote=&transports[next_transport++]; remote->address=0x64563812UL;
 assert(network_game_server_add_new_client(s,remote)); b=&s->client_machines[1];
 m=attach(2); network_game_server_handle_migration(s,b,&m,sizeof(m));
 assert(b->machine_index==2 && TEST_FLAG(b->flags,_network_client_machine_validated_bit) && writes==3); preserved();
 network_game_server_migration_detach(s,b); now+=120000; network_game_server_migration_expire_disconnected(s);
 assert(s->game.machine_count==1 && datums[2].quit_out_of_game_time==901);
 /* A partitioned former host rejoins as its original player after the new
 authority commits. It cannot reclaim the listener or a different owner slot. */
 setup(); s=network_game_server_adopt_match(&local_client,1); assert(s); global_server=s;
 a=&s->client_machines[0]; local=&transports[next_transport++]; local->address=IPV4_LOOPBACK_ADDRESS;
 assert(network_game_server_add_new_client(s,local));
 m=attach(1); network_game_server_handle_migration(s,a,&m,sizeof(m));
 network_game_server_migration_finish(s); assert(server_migration.disconnected_at[0] && !server_migration.adopting);
 remote=&transports[next_transport++]; remote->address=0x64563899UL;
 assert(network_game_server_add_new_client(s,remote)); b=&s->client_machines[1];
 m=attach(0); network_game_server_handle_migration(s,b,&m,sizeof(m));
 assert(!TEST_FLAG(b->flags,_network_client_machine_validated_bit));
 remote->address=0x64563810UL; network_game_server_handle_migration(s,b,&m,sizeof(m));
 assert(b->machine_index==0 && !server_migration.disconnected_at[0] && writes==3);
 assert(datums[0].identifier==100 && datums[0].score==17 && datums[0].quit_out_of_game_time==NONE);
 assert(machine_to_player_table[0][0]==0 && s->game.machine_count==3 && tick==900); preserved();
 /* Retire the old host, reuse its machine ID for a late join, then migrate
 again. Historical score data stays, while the new machine owns only its
 new player's inputs and must be eligible to reattach. */
 setup(); network_game_client_migration_remove_machine(&local_client,0);
 assert(machine_to_player_table[0][0]==NONE && datums[0].identifier==100 && datums[0].score==17);
 struct network_player fresh={0}; fresh.machine_index=0; fresh.player_list_index=NONE;
 assert(network_game_client_add_player_to_game(&local_client,&fresh));
 assert(fresh.player_list_index==3 && local_client.game.machines[0].machine_index==0 && local_client.game.machine_count==3);
 assert(machine_to_player_table[0][0]==3 && machine_to_player_table[0][1]==NONE && datums[0].score==17);
 client_migration.host_machine=2;
 s=network_game_server_adopt_match(&local_client,2); assert(s && s->game.machines[0].machine_index==0);
 global_server=s;
 struct network_migration_membership membership;
 assert(network_game_server_migration_machines(&membership,sizeof(membership)));
 assert(membership.machine_count==3 && membership.machines[0].machine_index==0 && membership.machines[2].machine_index==2);
 /* Negative control: the pre-fix promoted roster rejects the reused m0
 despite its current owner's correct source address and original session. */
 s->game.machines[0].machine_index=NONE; s->game.machine_count--;
 remote=&transports[next_transport++]; remote->address=0x64563810UL;
 assert(network_game_server_add_new_client(s,remote)); b=&s->client_machines[0];
 m=attach(0); m.epoch=2; network_game_server_handle_migration(s,b,&m,sizeof(m));
 assert(!TEST_FLAG(b->flags,_network_client_machine_validated_bit));
 /* A committed authoritative roster repairs membership without touching
 any live player datum, score, input binding, or clock. */
 local_client.game.machines[0].machine_index=NONE; local_client.game.machine_count--;
 assert(network_game_client_migration_set_machines(&membership,sizeof(membership)));
 assert(local_client.game.machines[0].machine_index==0 && local_client.game.machine_count==3);
 memcpy(s->game.machines,local_client.game.machines,sizeof(s->game.machines)); s->game.machine_count=local_client.game.machine_count;
 network_game_server_handle_migration(s,b,&m,sizeof(m));
 assert(TEST_FLAG(b->flags,_network_client_machine_validated_bit) && b->machine_index==0);
 assert(datums[0].identifier==100 && datums[0].score==17 && machine_to_player_table[0][0]==3 && tick==900);
 membership.machines[0].machine_index=1;
 assert(!network_game_client_migration_validate_machines(&membership,sizeof(membership)));
 assert(!network_game_client_migration_set_machines(&membership,sizeof(membership)) && local_client.game.machines[0].machine_index==0);
 membership.machines[0].machine_index=0; membership.machine_count=4;
 assert(!network_game_client_migration_validate_machines(&membership,sizeof(membership)));
 puts("migration managers: remote admission, identity/epoch gates, ACK framing, cohort resume, reused late-join roster and input ownership passed");
 return 0;
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix="halo-migration-managers-") as directory:
        folder = Path(directory)
        source, binary = folder / "test.c", folder / "test"
        source.write_text(BOUNDARY + WIRE + STUBS + FUNCTIONS + TESTS)
        subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
        # Reintroduce the original existing-server rejection. The retained
        # host case above must fail even when all other migration code works.
        original = function(GLOBALS, "create_global_network_game_server_from_migration")
        control = original.replace("{", "{\n\tif (global_network_game_server) return FALSE;", 1)
        source.write_text(BOUNDARY + WIRE + STUBS + FUNCTIONS.replace(original, control) + TESTS)
        subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(binary)], check=True)
        result = subprocess.run([str(binary)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        assert result.returncode != 0 and "Assertion" in result.stderr, result.stderr
        print("Original existing-host recovery rejection failed the regression as expected")
        # Restore the old epoch-zero removal guard. A lost RTC connection on
        # the still-running original host must retain the original roster.
        original = function(SERVER, "network_game_server_remove_disconnected_client")
        control = original.replace(
            "server_migration.epoch || (server->state == _network_game_server_state_ingame && web_match_migration_enabled())",
            "server_migration.epoch",
        )
        assert control != original
        source.write_text(BOUNDARY + WIRE + STUBS + FUNCTIONS.replace(original, control) + TESTS)
        subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(binary)], check=True)
        result = subprocess.run([str(binary)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        assert result.returncode != 0 and "Assertion" in result.stderr, result.stderr
        print("Original epoch-zero roster removal failed the regression as expected")
        # Restore initial-authority reattach rejection. A client must be able
        # to repair epoch zero without making the whole room change hosts.
        original = function(SERVER, "network_game_server_handle_migration")
        control = original.replace(
            "(!server_migration.epoch && !web_match_migration_enabled())",
            "!server_migration.epoch",
        )
        assert control != original
        source.write_text(BOUNDARY + WIRE + STUBS + FUNCTIONS.replace(original, control) + TESTS)
        subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(binary)], check=True)
        result = subprocess.run([str(binary)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        assert result.returncode != 0 and "Assertion" in result.stderr, result.stderr
        print("Original epoch-zero reattach rejection failed the regression as expected")
        original = function(SERVER, "network_game_server_add_new_client")
        control = original.replace(
            "if (!server_migration.epoch && !(server->state == _network_game_server_state_ingame && web_match_migration_enabled()))",
            "if (!server_migration.epoch)",
        )
        assert control != original
        source.write_text(BOUNDARY + WIRE + STUBS + FUNCTIONS.replace(original, control) + TESTS)
        subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(binary)], check=True)
        result = subprocess.run([str(binary)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        assert result.returncode != 0 and "Assertion" in result.stderr, result.stderr
        print("Original epoch-zero admission invalidation failed the regression as expected")


if __name__ == "__main__":
    main()
