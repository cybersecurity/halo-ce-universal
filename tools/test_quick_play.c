/* Compile the real production state machine against a deterministic engine
boundary. These tests exercise lifecycle, deadlines, cancellation and player
confirmation; multiplayer transport is covered by the separate relay tests. */
#define HALO_WEB 1
#define HALO_LINUX 1
#define HALO_QUICK_PLAY_TEST 1
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../port/linux/game/quick_play.c"
#include "../port/web/src/web_quick_play.c"

static struct network_game_client mock_client;
static struct network_game_server mock_server;
static struct network_game_client *client;
static struct network_game_server *server;
static unsigned long clock_ms, last_target;
static const char *mode, *target;
static short connection, join_result;
static int created, aborts, menus, add_requests, starts, searches, reports, maps;
static int quick_policy, policy_requests, countdown;
static int adopted, reattached, finished;
static boolean paused, migration_ready, cohort_ready, migration_supported;
static unsigned long engine_epoch;
static char last_phase[32];

unsigned long system_milliseconds(void) { return clock_ms; }
const char *config_string(const char *name) { return !strcmp(name, "network.quick_play") ? mode : target; }
double emscripten_get_now(void) { return (double)clock_ms; }
void web_js_post(int kind, const char *message)
{ assert(kind == 6); assert(sscanf(message, "{\"phase\":\"%31[^\"]", last_phase) == 1); reports++; }
struct network_game_client *global_network_game_client_get(void) { return client; }
struct network_game_server *global_network_game_server_get(void) { return server; }
void dispose_global_network_game_client(void) { client = NULL; }
void dispose_global_network_game_server(void) { server = NULL; }
boolean create_global_network_game_client(void) { created++; client = &mock_client; return TRUE; }
void player_ui_fast_setup_network_server(void)
{
	/* The real helper initializes its playlist immediately. A blank initial
	map makes it try unavailable Carousel before the pregame map change. */
	assert(!strcmp(web_quick_play_initial_map(), "levels\\test\\beavercreek\\beavercreek"));
	created++; client = &mock_client; server = &mock_server; connection = _game_connection_network_server;
}
void main_goto_main_menu(void) { menus++; }
void game_connection_set(short value) { connection = value; }
short game_connection(void) { return connection; }
void network_game_abort(void) { aborts++; }
void network_game_client_request_immediate_start(void) { starts++; }
void network_game_server_change_map_name(struct network_game_server *value, const char *path)
{ assert(value == server); assert(!strcmp(path, "levels\\test\\beavercreek\\beavercreek")); maps++; }
void network_game_server_change_game_variant(struct network_game_server *value, struct game_variant *variant)
{ assert(value == server && variant); }
boolean network_game_server_enable_quick_play(struct network_game_server *value)
{ assert(value == server); policy_requests++; return quick_policy; }
void network_game_server_pause_countdown(struct network_game_server *value, boolean paused)
{ assert(value == server && !paused); }
struct game_variant *game_engine_get_variant_by_name(struct game_variant *variant, const char *name)
{ assert(!strcmp(name, "slayer")); variant->unused = 0; return variant; }
void player_ui_set_game_variant(struct game_variant *variant) { assert(variant); }
void ui_widgets_close_all(void) {}
void *ui_widget_load_by_name_or_tag(const char *a, int b, void *c, int d, int e, int f, int g)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; return &mock_client; }
short network_game_client_get_state(struct network_game_client *value, short *data)
{ (void)data; return value->state; }
short network_game_client_get_error(struct network_game_client *value) { return value->error; }
short network_game_client_get_seconds_to_game_start(struct network_game_client *value)
{ assert(value == client); return (short)countdown; }
boolean network_game_client_has_local_player(struct network_game_client *value, short controller)
{ assert(controller == 0); return value->player; }
boolean network_game_client_add_player(struct network_game_client *value, short controller)
{ assert(value == client && controller == 0); add_requests++; return TRUE; }
boolean network_game_client_server_has_started_game(struct network_game_client *value)
{ (void)value; return FALSE; }
short network_game_client_quick_join(unsigned long address)
{ searches++; last_target = address; return join_result; }
boolean game_time_initialized(void) { return client && client->state == _network_game_client_state_ingame; }
boolean game_time_get_paused(void) { return paused; }
void game_time_set_paused(boolean value) { paused = value; }
boolean create_global_network_game_server_from_migration(unsigned long epoch)
{ adopted++; engine_epoch = epoch; server = &mock_server; connection = _game_connection_network_server; return migration_supported; }
boolean network_game_client_begin_migration(struct network_game_client *value, unsigned long address, unsigned long epoch)
{ assert(value == client); reattached++; last_target = address; engine_epoch = epoch; value->error = 0; return migration_supported; }
boolean network_game_client_migration_ready(struct network_game_client *value)
{ assert(value == client); return migration_ready; }
boolean network_game_server_migration_ready(struct network_game_server *value)
{ assert(value == server); return cohort_ready; }
void network_game_server_migration_finish(struct network_game_server *value)
{ assert(value == server); finished++; }

static void reset(const char *setting)
{
	memset(&quick_play, 0, sizeof(quick_play));
	memset(&mock_client, 0, sizeof(mock_client));
	client = NULL; server = NULL; mode = setting; target = "";
	clock_ms = 1000; last_target = 0; connection = 0; join_result = 0;
	cancel_requested = 0; migration_requested = 0; reconnect_requested = 0; hold_requested = 0; initial_epoch = 0;
	background_active = 0; background_drain_until = 0;
	created = aborts = menus = add_requests = starts = searches = reports = maps = 0;
	quick_policy = TRUE; policy_requests = 0; countdown = NONE;
	last_phase[0] = 0;
	adopted = reattached = finished = 0; paused = migration_ready = cohort_ready = FALSE;
	migration_supported = TRUE; engine_epoch = 0;
}
static void step(unsigned long elapsed, boolean menu)
{ clock_ms += elapsed; quick_play_update(menu); }
static void launch(const char *setting)
{ reset(setting); step(0, TRUE); step(2000, TRUE); assert(created == 1); }

int main(void)
{
	unsigned long address;
	/* Must match transport_client_start's SWAP4(socket address), not the
	JS ring's little-endian socket word; otherwise every elected host is skipped. */
	assert(quick_play_parse_address("100.86.56.19", &address) && address == 0x64563813UL);
	assert(quick_play_parse_address("10.1.2.3", &address) && address == 0x0A010203UL);
	assert(!quick_play_parse_address("100.86.56.19 trailing", &address));
	assert(!quick_play_parse_address("999999999999999999.1.1.1", &address));
	assert(!quick_play_parse_address("224.0.0.1", &address));
	assert(!quick_play_parse_address("0.0.0.0", &address));
	assert(!quick_play_parse_address("100.86.256.19", &address));
	assert(!quick_play_parse_address("100.86.56", &address));

	reset(""); step(1000000, TRUE); assert(!created && !reports);
	assert(!web_quick_play_pistol_starts());
	assert(!web_quick_play_initial_map()[0]);
	reset("host"); step(0, TRUE);
	assert(!web_quick_play_initial_map()[0]); /* settling, but no owned session */
	step(2000, TRUE); assert(created == 1);
	assert(!web_quick_play_initial_map()[0]); /* initial playlist already selected */
	reset("");
	/* Manual System Link has no quick-play mode, but must keep simulating
	while hidden so later players can discover and join its host. */
	assert(!web_multiplayer_active()); client = &mock_client;
	assert(!web_multiplayer_active()); connection = _game_connection_network_server;
	assert(web_multiplayer_active()); connection = _game_connection_network_client;
	assert(web_multiplayer_active()); client = NULL; assert(!web_multiplayer_active());
	reset("invalid"); step(0, TRUE); assert(!strcmp(last_phase, "error") && !created);
	reset("join"); target = "invalid"; step(0, TRUE); assert(!strcmp(last_phase, "error") && !created);
	reset("join"); step(0, FALSE); web_quick_play_cancel(); step(1, FALSE);
	assert(!strcmp(last_phase, "menu") && !created); step(1000000, TRUE); assert(!created);

	launch("join"); target = "100.86.56.19"; /* Target is captured only at launch. */
	step(499, TRUE); assert(!searches); step(1, TRUE); assert(searches == 1);
	step(60001, TRUE); assert(!strcmp(last_phase, "error") && aborts == 1);
	step(1000000, TRUE); assert(searches == 1 && created == 1 && aborts == 1);
	reset("join"); target = "100.86.56.19"; step(0, TRUE); step(2000, TRUE); step(500, TRUE);
	assert(last_target == 0x64563813UL);

	launch("join"); join_result = -1; step(500, TRUE);
	assert(!strcmp(last_phase, "error") && aborts == 1 && searches == 1);
	launch("join"); web_quick_play_cancel(); step(1, TRUE);
	assert(!strcmp(last_phase, "menu") && aborts == 1 && !searches);
	launch("join"); connection = _game_connection_local; step(1, TRUE);
	assert(!strcmp(last_phase, "menu") && !aborts); step(1000000, TRUE); assert(created == 1);

	/* A successful send is not an acknowledged player: never start without it. */
	launch("host"); mock_client.state = _network_game_client_state_pregame;
	step(500, TRUE); assert(add_requests == 1 && maps == 1);
	step(3000, TRUE); assert(!starts); step(120001, TRUE);
	assert(!starts && aborts == 1 && !strcmp(last_phase, "error"));

	launch("host"); mock_client.state = _network_game_client_state_pregame;
	step(500, TRUE); mock_client.player = TRUE; step(1, TRUE);
	step(2999, TRUE); assert(!starts); step(1, TRUE); assert(starts == 1);
	assert(web_quick_play_pistol_starts());
	step(999, TRUE); assert(starts == 1); step(1, TRUE); assert(starts == 2);
	countdown = 0; step(10000, TRUE); assert(starts == 2 && maps == 1 && policy_requests == 1);
	mock_client.state = _network_game_client_state_ingame; step(1, FALSE);
	assert(!strcmp(last_phase, "playing")); step(1000000, FALSE);
	assert(web_quick_play_pistol_starts());
	assert(!web_quick_play_initial_map()[0]);
	assert(starts == 2 && !aborts); web_quick_play_cancel(); step(1, FALSE);
	assert(!strcmp(last_phase, "menu") && aborts == 1);
	assert(!web_quick_play_pistol_starts());
	assert(!web_quick_play_initial_map()[0]);

	launch("host"); quick_policy = FALSE; mock_client.state = _network_game_client_state_pregame;
	step(500, TRUE); assert(!strcmp(last_phase, "error") && !starts && aborts == 1);
	launch("host"); mock_client.state = _network_game_client_state_pregame;
	step(500, TRUE); mock_client.player = TRUE; step(1, TRUE); step(3000, TRUE);
	assert(starts == 1); step(1000, TRUE); assert(starts == 2);
	step(120001, TRUE); assert(!strcmp(last_phase, "error") && starts == 2 && aborts == 1);

	/* A late join follows normal pregame/player acceptance, then enters the
	already running match without issuing a host start or scripted input. */
	launch("join"); join_result = 1; step(500, TRUE);
	mock_client.state = _network_game_client_state_pregame; step(500, TRUE);
	assert(add_requests == 1 && !starts); mock_client.player = TRUE; step(1, TRUE);
	mock_client.state = _network_game_client_state_ingame; step(1, FALSE);
	assert(!strcmp(last_phase, "playing") && !starts);
	assert(!web_quick_play_pistol_starts());
	mock_client.state = _network_game_client_state_postgame; step(1, FALSE);
	assert(!strcmp(last_phase, "menu") && aborts == 1);
	step(1000000, TRUE); assert(created == 1 && searches == 1 && !starts);

	/* Host loss holds the actual session until the elected host adopts it.
	   No abort, menu, map selection, lobby, new player or start is allowed. */
	launch("join"); mock_client.state = _network_game_client_state_ingame;
	step(1, FALSE); mock_client.error = 8; step(1, FALSE);
	assert(!strcmp(last_phase, "disconnected") && paused && !aborts && !menus);
	web_quick_play_migrate(1, 0x0302010aU, 1); step(1, FALSE);
	assert(!strcmp(last_phase, "migrating") && quick_play.host && adopted == 1 && reattached == 1);
	assert(last_target == 0x7f000001UL && engine_epoch == 1 && paused);
	step(1000, FALSE); assert(!finished && paused);
	cohort_ready = TRUE; step(1, FALSE); assert(finished == 1 && paused);
	step(1, FALSE); assert(finished == 1 && paused);
	migration_ready = TRUE; step(1, FALSE);
	assert(!strcmp(last_phase, "playing") && !paused && created == 1 && !maps && !starts && !aborts && !menus);
	assert(!web_quick_play_initial_map()[0]);

	/* A promoted host still has a local network client. If that connection
	   fails after takeover, preserve the world for another handoff as well. */
	mock_client.error = 8; step(1, FALSE);
	assert(!strcmp(last_phase, "disconnected") && paused && !aborts && !menus);
	migration_ready = cohort_ready = FALSE;
	web_quick_play_migrate(2, 0x0403020aU, 2); step(1, FALSE);
	assert(!quick_play.host && reattached == 2 && engine_epoch == 2 && paused);
	migration_ready = TRUE; step(1, FALSE);
	assert(!strcmp(last_phase, "playing") && !paused && created == 1 && !maps && !starts && !aborts && !menus);

	/* A survivor reconnects its existing player to the exact elected address. */
	launch("join"); mock_client.state = _network_game_client_state_ingame;
	step(1, FALSE); web_quick_play_hold(1); step(1, FALSE); assert(paused);
	web_quick_play_migrate(2, 0x0302010aU, 4); step(1, FALSE);
	assert(quick_play.target == 0x0a010203UL && !quick_play.host && engine_epoch == 4);
	assert(reattached == 1 && !adopted && paused && !aborts && !menus);
	web_quick_play_migrate(1, 0, 3); step(1, FALSE); assert(reattached == 1 && !adopted);
	web_quick_play_hold(0); step(1, FALSE); assert(paused); /* no premature release */
	migration_ready = TRUE; step(1, FALSE); assert(!paused && !strcmp(last_phase, "playing"));
	assert(created == 1 && !starts && !maps && !add_requests);

	/* A brief outage releases its hold without adopting or reconnecting. */
	launch("join"); mock_client.state = _network_game_client_state_ingame; step(1, FALSE);
	web_quick_play_hold(1); step(1, FALSE); assert(paused);
	web_quick_play_hold(0); step(1, FALSE);
	assert(!paused && !adopted && !reattached && !aborts && !menus);

	/* A single client repairs its original stream at epoch zero. The loaded
	   player/world remain; neither the host listener nor another player resets. */
	launch("join"); quick_play.target = 0x0a010203UL;
	mock_client.state = _network_game_client_state_ingame; step(1, FALSE);
	web_quick_play_reconnect(0x0302010aU, 0); step(1, FALSE);
	assert(quick_play.phase == QUICK_RECONNECTING && paused && reattached == 1 && engine_epoch == 0);
	assert(last_target == 0x0a010203UL && !adopted && !maps && !starts && !aborts && !menus);
	web_quick_play_hold(0); step(1, FALSE); assert(paused);
	migration_ready = TRUE; step(1, FALSE); assert(!paused && !strcmp(last_phase, "playing"));
	web_quick_play_reconnect(0x0403020aU, 0); step(1, FALSE); assert(reattached == 1);
	web_quick_play_reconnect(0x0302010aU, 1); step(1, FALSE); assert(reattached == 1);
	web_quick_play_reconnect(0x0302010aU, 0); web_quick_play_cancel(); step(1, FALSE);
	assert(!reconnect_requested && reattached == 1 && aborts == 1);

	/* Failure leaves the world paused. It must never silently reset the match. */
	launch("join"); mock_client.state = _network_game_client_state_ingame; step(1, FALSE);
	web_quick_play_hold(1); step(1, FALSE);
	web_quick_play_migrate(2, 0x0302010aU, 1); step(1, FALSE);
	step(45001, FALSE);
	assert(paused && !aborts && !menus);
	migration_ready = TRUE; step(1000, FALSE);
	assert(!paused && !strcmp(last_phase, "playing") && reattached == 1 && created == 1);

	launch("join"); mock_client.state = _network_game_client_state_ingame; step(1, FALSE);
	web_quick_play_hold(1); step(1, FALSE); migration_supported = FALSE;
	web_quick_play_migrate(2, 0x0302010aU, 1); step(1, FALSE);
	assert(paused && !aborts && !menus && !strcmp(last_phase, "migration-failed"));
	step(100000, FALSE); assert(paused && !aborts && !menus);

	/* Explicit Main menu wins over an election, even while hidden. */
	web_quick_play_migrate(1, 0, 2); web_quick_play_cancel(); step(1, FALSE);
	assert(!strcmp(last_phase, "menu") && !migration_requested && !paused);
	assert(!web_quick_play_take_cancel());
	web_quick_play_migrate(0, 123, 1); assert(!migration_requested);
	web_quick_play_migrate(2, 123, 0); assert(!migration_requested);
	web_quick_play_migrate(2, 0x0403020aU, 0x7fffffffU); assert(web_quick_play_background_active());
	{ int host; unsigned long next_target; unsigned int epoch;
	  assert(web_quick_play_take_migrate(&host, &next_target, &epoch));
	  assert(!host && next_target == 0x0a020304UL && epoch == 0x7fffffffU);
	  assert(!web_quick_play_take_migrate(&host, &next_target, &epoch)); }
	web_quick_play_set_address(0x0403020aU); assert(web_quick_play_address() == 0x0a020304UL);
	puts("Quick-play lifecycle, confirmation, targeting, timeout, cancellation and late-join tests passed.");
	return 0;
}
