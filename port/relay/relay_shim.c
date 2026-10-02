#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "posix.h"
#include "port_config.h"

unsigned long GetTickCount(void)
{
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (unsigned long)((unsigned long long)value.tv_sec * 1000 + value.tv_nsec / 1000000);
}

void platform_log(const char *format, ...)
{
    /* Native host logging can contain its invite: never forward it to logs. */
    (void)format;
}

int config_boolean(const char *name)
{
    return !strcmp(name, "network.online") || !strcmp(name, "debug.null_renderer");
}
long config_integer(const char *name) { (void)name; return 0; }
double config_real(const char *name) { (void)name; return 0; }
const char *config_string(const char *name)
{
    if (!strcmp(name, "network.signalling_brokers")) {
#ifdef HALO_RELAY_TEST
        const char *test_broker = getenv("HALO_RELAY_TEST_BROKER");
        if (test_broker) return test_broker;
#endif
        return "broker.emqx.io:1883,broker.hivemq.com:1883,test.mosquitto.org:1883";
    }
    if (!strcmp(name, "network.stun_servers")) {
#ifdef HALO_RELAY_TEST
        return "";
#else
        return "stun.l.google.com:19302,stun.cloudflare.com:3478";
#endif
    }
    return "";
}
int relay_candidate_allowed(unsigned long address, unsigned short port)
{
    uint32_t ip = ntohl((uint32_t)address);
    if (!port) return 0;
#ifdef HALO_RELAY_TEST
    if (getenv("HALO_RELAY_TEST_BROKER")) return 1;
#endif
    /* Public unicast only: no local, private, CGNAT, metadata, multicast,
       documentation, benchmarking or reserved ranges. */
    return !((ip >> 24) == 0 || (ip >> 24) == 10 || (ip >> 24) == 127 ||
        (ip & 0xffc00000u) == 0x64400000u || (ip & 0xffff0000u) == 0xa9fe0000u ||
        (ip & 0xfff00000u) == 0xac100000u || (ip & 0xffffff00u) == 0xc0000000u ||
        (ip & 0xffffff00u) == 0xc0000200u || (ip & 0xffff0000u) == 0xc0a80000u ||
        (ip & 0xffffff00u) == 0xc0586300u || (ip & 0xfffe0000u) == 0xc6120000u ||
        (ip & 0xffffff00u) == 0xc6336400u || (ip & 0xffffff00u) == 0xcb007100u ||
        ip >= 0xe0000000u);
}
int posix_upnp_forward_udp(unsigned short port, posix_ulong *address, unsigned short *external,
    char *error, int size)
{
    (void)port; (void)address; (void)external;
    if (size) snprintf(error, (size_t)size, "UPnP is disabled in the relay");
    return 0;
}
void posix_upnp_stop_forwarding_udp(unsigned short port) { (void)port; }
void p2p_discord_update(void) {}
void p2p_discord_set_hosting(const char *secret, int count, int maximum)
{ (void)secret; (void)count; (void)maximum; }
