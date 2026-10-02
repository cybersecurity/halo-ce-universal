/*
BROWSER.C

The game list (browser.h; configure.py --game-browser): hosted system link
games announced to the list server (network.browser_url: halo.milenko.org,
the community's) with their invites.

The game's server reports its game each frame (browser_host_update). While
p2p.c hosts it on the internet (it has an invite) and
network.list_hosted_games is on, the game is announced every
ANNOUNCE_INTERVAL, or a little after it changes (players join, the map
changes), and withdrawn when the reports stop or p2p.c stops hosting. The
server forgets a game it is not told about for a while, so a copy of the
game that quits without withdrawing drops off by itself.

The requests (posix.h's posix_browser_request) block, so they are made on a
thread of this file's, which the first report starts; the game's threads
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
	ANNOUNCE_INTERVAL = 20000,
	/* (a change announced this soon after the last announcement at most) */
	CHANGE_INTERVAL = 3000,
	/* the game's server stopped reporting: it no longer hosts */
	HOST_TIMEOUT = 3000,
	/* (asked again this long after a failure) */
	RETRY_INTERVAL = 15000,
	THREAD_INTERVAL = 250,
};

struct hosted_game
{
	unsigned short name[BROWSER_NAME_LENGTH];
	char map[BROWSER_MAP_LENGTH];
	short engine;
	short players;
	short maximum_players;
	int open;
	short score_limit;
	int teams;
};

static pthread_mutex_t browser_lock = PTHREAD_MUTEX_INITIALIZER;
/* (whether the thread was started: Windows's threads have no pthread_once) */
static int browser_started;

static struct
{
	/* hosting (the game's threads write, the browser thread reads) */
	struct hosted_game hosted;
	unsigned long host_report_time;
	int host_reported;
	int host_changed;

	/* the browser thread's own */
	char listed_invite[BROWSER_INVITE_LENGTH + 1];
	unsigned long announce_time;
	struct hosted_game announced;
} browser;

/* ---------- text */

static int elapsed(unsigned long since, unsigned long interval)
{
	return !since || p2p_now() - since >= interval;
}

/* UTF-16 to UTF-8 */
static void utf8_from_name(const unsigned short *name, int length, char *text, int size)
{
	int used = 0;
	int index;

	for (index = 0; index < length && name[index]; index++)
	{
		unsigned int character = name[index];
		char bytes[3];
		int count;

		/* (no surrogate pairs in the game's names) */
		if (character < 0x80)
		{
			bytes[0] = (char)character;
			count = 1;
		}
		else if (character < 0x800)
		{
			bytes[0] = (char)(0xC0 | (character >> 6));
			bytes[1] = (char)(0x80 | (character & 0x3F));
			count = 2;
		}
		else
		{
			bytes[0] = (char)(0xE0 | (character >> 12));
			bytes[1] = (char)(0x80 | ((character >> 6) & 0x3F));
			bytes[2] = (char)(0x80 | (character & 0x3F));
			count = 3;
		}
		if (used + count >= size)
			break;
		memcpy(text + used, bytes, (size_t)count);
		used += count;
	}
	text[used] = 0;
}

/* appends name=value, URL encoded */
static void form_add(char *form, int size, const char *name, const char *value)
{
	static const char digits[] = "0123456789ABCDEF";
	int used = (int)strlen(form);

	used += snprintf(form + used, (size_t)(size - used), "%s%s=", used ? "&" : "", name);
	for (; *value && used < size - 4; value++)
	{
		unsigned char character = (unsigned char)*value;

		if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
			(character >= '0' && character <= '9') || character == '-' || character == '_' || character == '.')
		{
			form[used++] = (char)character;
		}
		else
		{
			form[used++] = '%';
			form[used++] = digits[character >> 4];
			form[used++] = digits[character & 15];
		}
	}
	form[used] = 0;
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

/* ---------- hosting (the browser thread) */

static void withdraw(void)
{
	char url[512], form[128], response[256], error[256];

	if (!browser.listed_invite[0])
		return;
	server_url("/v1/withdraw", url, sizeof(url));
	form[0] = 0;
	form_add(form, sizeof(form), "invite", browser.listed_invite);
	posix_browser_request(url, form, NULL, response, sizeof(response), error, sizeof(error));
	platform_log("Game list: the game is no longer listed");
	browser.listed_invite[0] = 0;
}

static void announce(const char *invite, const struct hosted_game *game)
{
	char url[512], form[1024], response[256], error[256], text[128];
	int status;

	server_url("/v1/announce", url, sizeof(url));
	form[0] = 0;
	form_add(form, sizeof(form), "invite", invite);
	utf8_from_name(game->name, BROWSER_NAME_LENGTH, text, sizeof(text));
	form_add(form, sizeof(form), "name", text);
	form_add(form, sizeof(form), "map", game->map);
	snprintf(text, sizeof(text), "%d", game->engine);
	form_add(form, sizeof(form), "engine", text);
	snprintf(text, sizeof(text), "%d", game->players);
	form_add(form, sizeof(form), "players", text);
	snprintf(text, sizeof(text), "%d", game->maximum_players);
	form_add(form, sizeof(form), "maximum_players", text);
	form_add(form, sizeof(form), "open", game->open ? "1" : "0");
	snprintf(text, sizeof(text), "%d", game->score_limit);
	form_add(form, sizeof(form), "score_limit", text);
	form_add(form, sizeof(form), "teams", game->teams ? "1" : "0");
	snprintf(text, sizeof(text), "%d", HALO_PORT_NETWORK_VERSION);
	form_add(form, sizeof(form), "version", text);

	status = posix_browser_request(url, form, NULL, response, sizeof(response), error, sizeof(error));
	browser.announce_time = p2p_now();
	browser.announced = *game;
	if (status == 200)
	{
		if (strcmp(browser.listed_invite, invite))
			platform_log("Game list: the game is listed on %s", config_string("network.browser_url"));
		snprintf(browser.listed_invite, sizeof(browser.listed_invite), "%s", invite);
	}
	else
	{
		response[strcspn(response, "\r\n")] = 0;
		platform_log("Game list: could not list the game (%s)", status ? response : error);
	}
}

static void update_hosting(void)
{
	char invite[BROWSER_INVITE_LENGTH + 1];
	struct hosted_game game;
	int reported, changed, hosting;

	pthread_mutex_lock(&browser_lock);
	reported = browser.host_reported && !elapsed(browser.host_report_time, HOST_TIMEOUT);
	changed = browser.host_changed;
	game = browser.hosted;
	browser.host_changed = 0;
	pthread_mutex_unlock(&browser_lock);

	hosting = reported && config_boolean("network.list_hosted_games") &&
		p2p_hosting_invite(invite, sizeof(invite));
	if (!hosting)
	{
		withdraw();
		return;
	}
	/* (a new invite: the old one's listing withdrawn first) */
	if (browser.listed_invite[0] && strcmp(browser.listed_invite, invite))
		withdraw();
	if (!browser.listed_invite[0]
		? elapsed(browser.announce_time, browser.announce_time ? RETRY_INTERVAL : 0)
		: elapsed(browser.announce_time, ANNOUNCE_INTERVAL) ||
			((changed || memcmp(&game, &browser.announced, sizeof(game))) &&
				elapsed(browser.announce_time, CHANGE_INTERVAL)))
	{
		announce(invite, &game);
	}
}

static void *browser_thread(void *unused)
{
	(void)unused;
	for (;;)
	{
		if (config_string("network.browser_url")[0])
			update_hosting();
		Sleep(THREAD_INTERVAL);
	}
	return NULL;
}

/* a copy of the game that quits while its game is listed takes it off the
list (without this the server drops it only once it stops hearing of it) */
static void withdraw_at_exit(void)
{
	/* (the browser thread may be mid-request: the listing is withdrawn by
	whichever of the two gets there) */
	if (browser.listed_invite[0])
		withdraw();
}

static void start_thread(void)
{
	pthread_t thread;

	atexit(withdraw_at_exit);
	if (pthread_create(&thread, NULL, browser_thread, NULL) == 0)
		pthread_detach(thread);
	else
		platform_log("Game list: could not start its thread");
}

/* ---------- public code */

void browser_host_update(const unsigned short *name, const char *map, short engine, short players,
	short maximum_players, int open, short score_limit, int teams)
{
	struct hosted_game game;
	int start;

	pthread_mutex_lock(&browser_lock);
	start = !browser_started;
	browser_started = 1;
	pthread_mutex_unlock(&browser_lock);
	if (start)
		start_thread();
	memset(&game, 0, sizeof(game));
	memcpy(game.name, name, sizeof(game.name));
	snprintf(game.map, sizeof(game.map), "%s", map);
	game.engine = engine;
	game.players = players;
	game.maximum_players = maximum_players;
	game.open = open != 0;
	game.score_limit = score_limit;
	game.teams = teams != 0;

	pthread_mutex_lock(&browser_lock);
	if (memcmp(&game, &browser.hosted, sizeof(game)))
	{
		browser.hosted = game;
		browser.host_changed = 1;
	}
	browser.host_reported = 1;
	browser.host_report_time = p2p_now();
	pthread_mutex_unlock(&browser_lock);
}

#endif
