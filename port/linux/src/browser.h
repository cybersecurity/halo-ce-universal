/*
BROWSER.H

The game list (configure.py --game-browser, HALO_GAME_BROWSER): a system
link game hosted on the internet (p2p.c: it has an invite) is announced to
the list server, network.browser_url (halo.milenko.org), with its invite,
for players to find and join it there; turned off with
network.list_hosted_games. Games are still joined only through their
invites. See browser.c.
*/

#ifndef __BROWSER_H
#define __BROWSER_H

/* an invite's code: the host's key hash and the token, in hexadecimal
(p2p_internal.h's P2P_LINK_SIZE, without "halo://join/") */
#define BROWSER_INVITE_LENGTH 64
#define BROWSER_NAME_LENGTH 16
#define BROWSER_MAP_LENGTH 64

/* the hosted game, as its advertisement describes it, each frame its
server runs (network_server_manager.c); a game whose reports stop is
withdrawn */
void browser_host_update(const unsigned short *name, const char *map, short engine, short players,
	short maximum_players, int open, short score_limit, int teams);

#endif
