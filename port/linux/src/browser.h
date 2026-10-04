/*
BROWSER.H

The game list (configure.py --game-browser, HALO_GAME_BROWSER): the server
browser's second source, beside the public games' signed listings
(p2p_lobby.c). The games listed on a list server, network.browser_url
(halo.milenko.org by default), each with its invite, are merged into Join
Game > Server Browser's list (menu_functions.c): a game both list is shown
once, as its listing. A game is joined through its invite, as a listing's
is. See browser.c.
*/

#ifndef __BROWSER_H
#define __BROWSER_H

/* the game list server read by default (the setting network.browser_url,
or HALO_NET_BROWSER, points it elsewhere; empty turns it off):
halo.milenko.org, run by Milenko (ChupathingyCE's developer), which lists
the internet games of any build that hosts them (its API:
https://halo.milenko.org/api). The list is read with a plain GET of
/v1/games.txt while the server browser is open: nothing about the player
is sent */
#define BROWSER_DEFAULT_URL "https://halo.milenko.org"

/* an invite's code: the host's key hash and the token, in hexadecimal
(p2p_internal.h's P2P_LINK_SIZE, without "halo://join/") */
#define BROWSER_INVITE_LENGTH 64
#define BROWSER_NAME_LENGTH 16
#define BROWSER_MAP_LENGTH 64
#define BROWSER_MAXIMUM_GAMES 64
/* the players of a listed game's roster that are kept */
#define BROWSER_LISTED_ROSTER 16

/* a player of a game's roster */
struct browser_roster_player
{
	/* (UTF-16, as the game's names) */
	unsigned short name[12];
	/* its team, -1 in a game without teams */
	short team;
};

struct browser_game
{
	char invite[BROWSER_INVITE_LENGTH + 1];
	/* (UTF-16, as the game's names) */
	unsigned short name[BROWSER_NAME_LENGTH];
	char map[BROWSER_MAP_LENGTH];
	short engine;
	short players;
	short maximum_players;
	unsigned char open;
	unsigned char teams;
	unsigned short version;
	short score_limit;
	/* who is in it, as its host announces it (none from hosts that do
	not); roster_count may be more than the players kept */
	short roster_count;
	struct browser_roster_player roster[BROWSER_LISTED_ROSTER];
};

/* the listed games, asking the server for the list again if the last one
is more than a few seconds old: those of this machine's network version,
without this machine's own. Returns their count. */
int browser_get_games(struct browser_game *games, int maximum_count);

#endif
