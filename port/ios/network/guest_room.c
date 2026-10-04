/* Native ARM guest adapter for the browser engine's quick-play controller. */
#include "guest_host.h"
#include <time.h>
#include "p2p.h"
#include "posix.h"
#include <string.h>
#include "../../web/src/web_quick_play.c"
double emscripten_get_now(void) {
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * 1000.0 + now.tv_nsec / 1000000.0;
}
void web_js_post(int kind, const char *text) { if (kind == 6) host_ios_room_report(text); }
int web_net_host_ping(unsigned int address, unsigned int host, unsigned int epoch) {
    return host_ios_room_ping(address, host, epoch);
}
void ios_quick_play_reset(void);
void ios_quick_play_begin(int host, unsigned long target, unsigned int epoch);
void ios_room_open(void) {
    web_quick_play_cancel();web_quick_play_take_cancel();
    ios_quick_play_reset();
    if(host_ios_browser_mode()) { host_ios_room_open(); return; }
    p2p_initialize(posix_local_ipv4_address());
    p2p_lobby_browse(1);
    host_ios_browser_open();
    struct p2p_listing pending; int connecting=0; double deadline=0;
    for (;;) {
        struct p2p_listing listings[256], chosen;
        if(connecting) {
            unsigned long address=0;
            if(p2p_peer_address(pending.identifier,&address)) { ios_quick_play_begin(0,quick_play_engine_address(address),0); break; }
            if(emscripten_get_now()>deadline) { connecting=0; p2p_lobby_mark_failed(pending.identifier); host_ios_browser_error("The server did not respond. It may be offline or unreachable from this network."); }
        }
        int count=p2p_lobby_games(listings,256);
        int action=host_ios_browser_update(listings,count,&chosen);
        if(action==2) { p2p_lobby_refresh(); continue; }
        if(action<0)break;
        if(action==1) {
            if(strstr(chosen.map,"@ce")) {host_ios_browser_error("This server requires a Halo PC / Custom Edition map, which this build cannot load.");continue;}
            if(!chosen.open || chosen.player_count>=chosen.maximum_player_count) {
                host_ios_browser_error("This server is full or is not accepting players."); continue;
            }
            if(!p2p_join_invite(chosen.invite)) {
                host_ios_browser_error("Could not connect to this server's invite. Refresh and try again."); continue;
            }
            pending=chosen;connecting=1;deadline=emscripten_get_now()+60000;
        }
        struct timespec pause={0,10000000};nanosleep(&pause,0);
    }
    p2p_lobby_browse(0);
    host_ios_browser_close();
}
void ios_room_pump(void) {
    unsigned int c[4];
    for (int count = 0; count < 32 && host_ios_room_poll(c); count++) {
        switch (c[0]) {
        case 1: web_quick_play_set_address(c[1]); web_quick_play_set_epoch(c[2]); break;
        case 2: web_quick_play_hold(c[1]); break;
        case 3: web_quick_play_migrate(c[1], c[2], c[3]); break;
        case 4: web_quick_play_reconnect(c[1], c[2]); break;
        case 5: web_quick_play_cancel(); break;
        case 7: { void ios_browser_test_open(void); ios_browser_test_open(); break; }
        case 6: ios_quick_play_begin(c[1]==1,quick_play_engine_address(c[2]),c[3]); break;
        }
    }
}
