/*
BROWSER.C

The game list (browser.h; configure.py --game-browser): the games listed on
the list server (network.browser_url: halo.milenko.org by default), each
with its invite, for the server browser to show beside the public games'
listings (p2p_lobby.c).

browser_get_games asks for the list (GET /v1/games.txt) when the last one
is more than LIST_INTERVAL old, and returns what it has meanwhile. Only
games of this machine's network version are kept (the others could not be
joined), and never this machine's own. A game is joined through its invite,
as an invite link would join it (menu_functions.c): the list only hands
out invites.

The requests (posix.h's posix_browser_request) block, so they are made on a
thread of this file's, which the first call starts; the game's threads
only exchange state with it under the lock.
*/

#ifdef HALO_GAME_BROWSER

#include "platform.h"
#include "posix.h"
#include "port_config.h"
#include "p2p.h"
#include "p2p_internal.h"
#include "browser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum
{
	LIST_INTERVAL = 5000,
	/* (asked again this long after a failure) */
	RETRY_INTERVAL = 15000,
	THREAD_INTERVAL = 250,
	RESPONSE_SIZE = 32768,
	/* a line of the list: invite name map engine players maximum_players
	open version age score_limit teams roster (tab separated; a list
	server from before the last ones leaves them out) */
	LIST_FIELDS = 12,
	LIST_REQUIRED_FIELDS = 8,
};

static pthread_mutex_t browser_lock = PTHREAD_MUTEX_INITIALIZER;
/* (whether the thread was started: Windows's threads have no pthread_once) */
static int browser_started;

static struct
{
	/* the list (the game's threads read it, the browser thread writes it) */
	int list_wanted;
	unsigned long list_time;
	int list_failed;
	int game_count;
	struct browser_game games[BROWSER_MAXIMUM_GAMES];
} browser;

/* ---------- text */

static int elapsed(unsigned long since, unsigned long interval)
{
	return !since || p2p_now() - since >= interval;
}

/* UTF-8 to UTF-16, into a name of the game's length */
static void name_from_utf8(const char *text, unsigned short *name, int length)
{
	const unsigned char *cursor = (const unsigned char *)text;
	int used = 0;

	while (*cursor && used < length - 1)
	{
		unsigned int character;

		if (*cursor < 0x80)
			character = *cursor++;
		else if ((*cursor & 0xE0) == 0xC0 && cursor[1])
		{
			character = ((cursor[0] & 0x1Fu) << 6) | (cursor[1] & 0x3Fu);
			cursor += 2;
		}
		else if ((*cursor & 0xF0) == 0xE0 && cursor[1] && cursor[2])
		{
			character = ((cursor[0] & 0x0Fu) << 12) | ((cursor[1] & 0x3Fu) << 6) | (cursor[2] & 0x3Fu);
			cursor += 3;
		}
		else
		{
			/* (a longer sequence, or a broken one: a question mark) */
			character = '?';
			cursor++;
			while ((*cursor & 0xC0) == 0x80)
				cursor++;
		}
		name[used++] = (unsigned short)character;
	}
	while (used < length)
		name[used++] = 0;
}

static void server_url(const char *path, char *url, int size)
{
	const char *base = config_string("network.browser_url");
	size_t length = strlen(base);

	/* (with or without the final slash) */
	while (length && base[length - 1] == '/')
		length--;
	snprintf(url, (size_t)size, "%.*s%s", (int)length, base, path);
}

/* ---------- the list (the browser thread) */

/* a listed game's roster, from the list's "team:name|team:name" */
static void parse_roster(char *text, struct browser_game *game)
{
	char *entry = text;

	while (entry && *entry)
	{
		char *next = strchr(entry, '|');
		char *colon;

		if (next)
			*next++ = 0;
		colon = strchr(entry, ':');
		if (colon && colon[1])
		{
			*colon = 0;
			if (game->roster_count < BROWSER_LISTED_ROSTER)
			{
				struct browser_roster_player *player = &game->roster[game->roster_count];

				name_from_utf8(colon + 1, player->name, 12);
				player->team = (short)atoi(entry);
			}
			game->roster_count++;
		}
		entry = next;
	}
}

static int parse_game(char *line, struct browser_game *game)
{
	char *fields[LIST_FIELDS];
	int count = 0;
	char *cursor = line;

	line[strcspn(line, "\r")] = 0;
	while (count < LIST_FIELDS)
	{
		fields[count++] = cursor;
		cursor = strchr(cursor, '\t');
		if (!cursor)
			break;
		*cursor++ = 0;
	}
	if (count < LIST_REQUIRED_FIELDS || strlen(fields[0]) != BROWSER_INVITE_LENGTH)
		return 0;
	memset(game, 0, sizeof(*game));
	snprintf(game->invite, sizeof(game->invite), "%s", fields[0]);
	name_from_utf8(fields[1], game->name, BROWSER_NAME_LENGTH);
	snprintf(game->map, sizeof(game->map), "%s", fields[2]);
	game->engine = (short)atoi(fields[3]);
	game->players = (short)atoi(fields[4]);
	game->maximum_players = (short)atoi(fields[5]);
	game->open = (unsigned char)(atoi(fields[6]) != 0);
	game->version = (unsigned short)atoi(fields[7]);
	/* (a list server from before these: none) */
	if (count >= 11)
	{
		game->score_limit = (short)atoi(fields[9]);
		game->teams = (unsigned char)(atoi(fields[10]) != 0);
	}
	if (count >= 12)
		parse_roster(fields[11], game);
	return 1;
}

static void update_list(void)
{
	static char response[RESPONSE_SIZE];
	char url[512], error[256];
	/* ("halo://join/" and the invite's digits) */
	char own[16 + BROWSER_INVITE_LENGTH];
	const char *own_invite;
	struct browser_game *games;
	int count = 0;
	int status;
	char *line;
	int wanted;

	pthread_mutex_lock(&browser_lock);
	wanted = browser.list_wanted &&
		elapsed(browser.list_time, browser.list_failed ? RETRY_INTERVAL : LIST_INTERVAL);
	browser.list_wanted = 0;
	pthread_mutex_unlock(&browser_lock);
	if (!wanted)
		return;

	server_url("/v1/games.txt", url, sizeof(url));
	status = posix_browser_request(url, NULL, NULL, response, sizeof(response), error, sizeof(error));
	games = malloc(sizeof(*games) * BROWSER_MAXIMUM_GAMES);
	if (!games)
		return;
	/* (the game this machine hosts, if it is listed: not shown to itself) */
	own_invite = p2p_invite_link(own, sizeof(own)) && strchr(own, '/') ? strrchr(own, '/') + 1 : "";
	if (status == 200)
	{
		for (line = strtok(response, "\n"); line && count < BROWSER_MAXIMUM_GAMES; line = strtok(NULL, "\n"))
		{
			if (parse_game(line, &games[count]) && games[count].version == HALO_PORT_NETWORK_VERSION &&
				strcmp(games[count].invite, own_invite))
			{
				count++;
			}
		}
	}
	else
	{
		if (status)
			platform_log("Game list: could not get the list (HTTP %d)", status);
		else
			platform_log("Game list: could not get the list (%s)", error);
	}
	pthread_mutex_lock(&browser_lock);
	browser.list_time = p2p_now();
	browser.list_failed = status != 200;
	if (status == 200)
	{
		memcpy(browser.games, games, sizeof(*games) * (size_t)count);
		browser.game_count = count;
	}
	pthread_mutex_unlock(&browser_lock);
	free(games);
}

static void *browser_thread(void *unused)
{
	(void)unused;
	for (;;)
	{
		if (config_string("network.browser_url")[0])
			update_list();
		Sleep(THREAD_INTERVAL);
	}
	return NULL;
}

static void start_thread(void)
{
	pthread_t thread;

	if (pthread_create(&thread, NULL, browser_thread, NULL) == 0)
		pthread_detach(thread);
	else
		platform_log("Game list: could not start its thread");
}

/* ---------- public code */

int browser_get_games(struct browser_game *games, int maximum_count)
{
	int count;
	int start;

	pthread_mutex_lock(&browser_lock);
	start = !browser_started;
	browser_started = 1;
	browser.list_wanted = 1;
	count = browser.game_count < maximum_count ? browser.game_count : maximum_count;
	memcpy(games, browser.games, sizeof(*games) * (size_t)count);
	pthread_mutex_unlock(&browser_lock);
	if (start)
		start_thread();
	return count;
}

#endif
