#ifndef HALO_WEB_PING_H
#define HALO_WEB_PING_H

/* Native transport IPv4 order; returns RTT in ms, or -1 when unavailable. */
int web_net_host_ping(unsigned int address, unsigned int host, unsigned int epoch);

#endif
