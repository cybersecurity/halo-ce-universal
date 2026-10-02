/* Minimal engine boundary for the production quick-play state-machine test. */
#ifndef QUICK_PLAY_TEST_STUBS_H
#define QUICK_PLAY_TEST_STUBS_H
#include <stddef.h>
typedef int boolean;
typedef unsigned short word;
#define TRUE 1
#define FALSE 0
#define NONE (-1)
#include "networking/network_client_manager.h"
enum { _game_connection_local, _game_connection_network_client, _game_connection_network_server };
struct network_game_client { short state, error; boolean player; };
struct network_game_server { int unused; };
struct game_variant { int unused; };
unsigned long system_milliseconds(void);
struct network_game_client *global_network_game_client_get(void);
struct network_game_server *global_network_game_server_get(void);
void dispose_global_network_game_client(void);
void dispose_global_network_game_server(void);
boolean create_global_network_game_client(void);
void player_ui_fast_setup_network_server(void);
void main_goto_main_menu(void);
void game_connection_set(short);
short game_connection(void);
void network_game_abort(void);
void network_game_client_request_immediate_start(void);
void network_game_server_change_map_name(struct network_game_server *, const char *);
void network_game_server_change_game_variant(struct network_game_server *, struct game_variant *);
boolean network_game_server_enable_quick_play(struct network_game_server *);
void network_game_server_pause_countdown(struct network_game_server *, boolean);
struct game_variant *game_engine_get_variant_by_name(struct game_variant *, const char *);
void player_ui_set_game_variant(struct game_variant *);
void ui_widgets_close_all(void);
void *ui_widget_load_by_name_or_tag(const char *, int, void *, int, int, int, int);
boolean game_time_initialized(void);
boolean game_time_get_paused(void);
void game_time_set_paused(boolean);
boolean create_global_network_game_server_from_migration(unsigned long);
boolean network_game_client_begin_migration(struct network_game_client *, unsigned long, unsigned long);
boolean network_game_client_migration_ready(struct network_game_client *);
boolean network_game_server_migration_ready(struct network_game_server *);
void network_game_server_migration_finish(struct network_game_server *);
#endif
