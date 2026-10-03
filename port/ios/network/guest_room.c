/* Native ARM guest adapter for the browser engine's quick-play controller. */
#include "guest_host.h"
#include <time.h>
#include "../../web/src/web_quick_play.c"
double emscripten_get_now(void) {
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * 1000.0 + now.tv_nsec / 1000000.0;
}
void web_js_post(int kind, const char *text) { if (kind == 6) host_ios_room_report(text); }
int web_net_host_ping(unsigned int address, unsigned int host, unsigned int epoch) {
    return host_ios_room_ping(address, host, epoch);
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
        }
    }
}
