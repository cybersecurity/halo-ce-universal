/*
P2P_LOBBY.C

Internet play's public games (the server browser, Join Game > Server
Browser): a public game's host publishes a signed listing of it through the
signalling brokers (p2p_signal.c), and browsers gather the listings and join
a game by the invite a listing holds, as an invite link would.

The listing (network byte order) is signed with the host's Ed25519 key,
whose X25519 form's hash is the invite's host part (p2p.c), so no one but
the host can list its invite, alter its listing, or list another's under
false details:

	"HL", format 1, the lobby version (2: HALO_PORT_NETWORK_VERSION; browsers
	hide others), flags (open, under way, teams, closed), sequence (4: newest
	wins), Unix time (4), the Ed25519 key (32), the invite's token (16),
	players, most players, the gametype's engine (1 each), the game's name,
	map and gametype (a length byte, then up to 32, 32 and 24 characters of
	printable ASCII), a stamp (8, reserved: zero), and the signature (64) of
	"hceu-lobby-1" and all before it.

Each host has a slot, hceu/3/lobby/s/<its key's hash in hex>: its listing,
retained on every broker (and expiring there after LISTING_EXPIRY seconds
on MQTT 5), and the broker's will (set at the connection, p2p_signal.c)
clears it if the host vanishes. Browsers subscribe to every slot and ask the
hosts to publish again (hceu/3/lobby/q). Brokers are other people's: what
one deletes or keeps is no authority, so
- a host publishes every REPUBLISH_INTERVAL, and sooner (at most once each
  TRIGGER_INTERVAL) when its game changes, when asked, and when its slot is
  heard to hold anything but its listing (cleared, another's, or an older
  one of its own): a deletion is undone in seconds;
- a browser takes only a signed listing whose key's hash is its slot's,
  keeps the newest of each host (by sequence), drops one not heard in
  GAME_EXPIRY (on its own clock; a retained copy is taken only if its time
  is within RETAINED_WINDOW of this machine's, from a host that died and
  left it), takes a host's closing listing (a tombstone) as its end, and
  ignores an emptied slot (a wipe is not a delete).
A host that stops publishes a tombstone, then clears its slot. Going private
makes a new invite (p2p.c), so a listing seen before lets no one in.

Checking signatures takes work, and anyone can send listings: at most
VERIFY_BUDGET milliseconds of it each pass of the p2p thread, from a queue
of MAXIMUM_QUEUED; the rest is dropped (hosts publish again).
*/

#ifdef HALO_RELAY
#include "relay_platform.h"
#else
#include "platform.h"
#endif
#include "posix.h"
#include "port_config.h"
#include "p2p_internal.h"
#include "halo_port_limits.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum
{
	LISTING_FORMAT = 1,
	_listing_open = 1,
	_listing_in_progress = 2,
	_listing_has_teams = 4,
	/* a tombstone: the host stopped */
	_listing_closed = 8,
	/* (reserved) */
	_listing_password = 16,
	STAMP_SIZE = 8,
	/* the signed part's end: everything but the signature */
	MAXIMUM_LISTING_SIZE = 2 + 1 + 2 + 1 + 4 + 4 + P2P_KEY_SIZE + P2P_TOKEN_SIZE + 3 +
		(1 + P2P_LISTING_NAME_SIZE) + (1 + P2P_LISTING_MAP_SIZE) + (1 + P2P_LISTING_GAMETYPE_SIZE) + STAMP_SIZE +
		P2P_SIGNATURE_SIZE,
	MINIMUM_LISTING_SIZE = 2 + 1 + 2 + 1 + 4 + 4 + P2P_KEY_SIZE + P2P_TOKEN_SIZE + 3 + 3 + STAMP_SIZE +
		P2P_SIGNATURE_SIZE,
	MAXIMUM_GAMES = 256,
	MAXIMUM_TOMBSTONES = 256,
	MAXIMUM_QUEUED = 64,

	/* milliseconds */
	REPUBLISH_INTERVAL = 30000,
	TRIGGER_INTERVAL = 5000,
	QUERY_JITTER = 2000,
	GAME_EXPIRY = 90000,
	TOMBSTONE_TIME = 600000,
	VERIFY_BUDGET = 2,
	/* seconds */
	RETAINED_WINDOW = 600,
};

static const char signature_label[] = "hceu-lobby-1";

/* a listing, read */
struct listing
{
	int version;
	int flags;
	unsigned long sequence;
	unsigned long time;
	unsigned char key[P2P_KEY_SIZE];
	unsigned char token[P2P_TOKEN_SIZE];
	int player_count, maximum_player_count, engine_type;
	char name[P2P_LISTING_NAME_SIZE + 1];
	char map[P2P_LISTING_MAP_SIZE + 1];
	char gametype[P2P_LISTING_GAMETYPE_SIZE + 1];
	/* the signed part's size */
	int signed_size;
};

struct game
{
	int used;
	unsigned char key_hash[P2P_KEY_HASH_SIZE];
	unsigned long sequence;
	unsigned long heard_time;
	/* as it came (one the same again needs no check) */
	unsigned char payload[MAXIMUM_LISTING_SIZE];
	int payload_size;
	struct p2p_listing listing;
};

struct tombstone
{
	unsigned char key_hash[P2P_KEY_HASH_SIZE];
	unsigned long sequence;
	unsigned long time;
};

struct queued
{
	unsigned char key_hash[P2P_KEY_HASH_SIZE];
	unsigned char payload[MAXIMUM_LISTING_SIZE];
	int size;
	int retained;
};

static struct
{
	/* hosting: the game as the game's server tells it
	(p2p_set_game_listing), and whether it is public (p2p_set_hosting_public) */
	int public;
	char name[P2P_LISTING_NAME_SIZE + 1];
	char map[P2P_LISTING_MAP_SIZE + 1];
	char gametype[P2P_LISTING_GAMETYPE_SIZE + 1];
	int engine_type;
	int flags;
	int player_count, maximum_player_count;
	/* listed: with this token; its listing as last published */
	int listed;
	unsigned char token[P2P_TOKEN_SIZE];
	unsigned long sequence;
	unsigned char listing[MAXIMUM_LISTING_SIZE];
	int listing_size;
	unsigned long published_time;
	/* a publish wanted (the game changed, a query, the slot needs mending),
	not before this */
	int republish_wanted;
	unsigned long republish_time;

	/* browsing */
	int browsing;
	int query_wanted;
	struct queued queue[MAXIMUM_QUEUED];
	int queue_count;
	struct game games[MAXIMUM_GAMES];
	struct tombstone tombstones[MAXIMUM_TOMBSTONES];
	int next_tombstone;
	/* the identifiers of games joining failed */
	unsigned char failed[MAXIMUM_GAMES][P2P_IDENTIFIER_SIZE];
	int failed_count;
} lobby;

static int elapsed(unsigned long since, unsigned long time)
{
	return (unsigned int)(p2p_now() - since) >= (unsigned int)time;
}

/* printable ASCII, as the menus' font has it (others: '?'), cut to size */
static void sanitize(char *destination, int size, const char *source, int source_size)
{
	int index;

	for (index = 0; index < size && index < source_size && source[index]; index++)
	{
		unsigned char character = (unsigned char)source[index];

		destination[index] = character >= 0x20 && character < 0x7F ? (char)character : '?';
	}
	destination[index] = 0;
}

void p2p_lobby_slot_topic(const unsigned char *key_hash, char *topic, int size)
{
	char text[2 * P2P_KEY_HASH_SIZE + 1];

	p2p_hex(key_hash, P2P_KEY_HASH_SIZE, text);
	snprintf(topic, (size_t)size, "%s%s", P2P_LOBBY_SLOT_PREFIX, text);
}

/* the hash of an Ed25519 key's X25519 form (an invite's host part); 0 if
the key has a small order */
static int signing_key_hash(const unsigned char *key, unsigned char *hash)
{
	unsigned char x25519[P2P_KEY_SIZE];

	if (!p2p_ed25519_to_x25519(key, x25519))
		return 0;
	p2p_key_hash(x25519, hash);
	return 1;
}

/* ---------- the listing */

static void put_long(unsigned char *bytes, unsigned long value)
{
	bytes[0] = (unsigned char)(value >> 24);
	bytes[1] = (unsigned char)(value >> 16);
	bytes[2] = (unsigned char)(value >> 8);
	bytes[3] = (unsigned char)value;
}

static unsigned long get_long(const unsigned char *bytes)
{
	return (unsigned long)bytes[0] << 24 | (unsigned long)bytes[1] << 16 | (unsigned long)bytes[2] << 8 | bytes[3];
}

static int put_text(unsigned char *bytes, const char *text, int maximum)
{
	int length = (int)strlen(text);

	length = length > maximum ? maximum : length;
	bytes[0] = (unsigned char)length;
	memcpy(bytes + 1, text, (size_t)length);
	return 1 + length;
}

/* the listing of the game hosted, signed: flags (_listing_closed for a
tombstone) */
static int listing_make(unsigned char *bytes, int flags)
{
	unsigned char *signed_part = bytes;
	unsigned char data[sizeof(signature_label) - 1 + MAXIMUM_LISTING_SIZE];
	int size = 0;

	bytes[size++] = 'H';
	bytes[size++] = 'L';
	bytes[size++] = LISTING_FORMAT;
	bytes[size++] = (unsigned char)(HALO_PORT_NETWORK_VERSION >> 8);
	bytes[size++] = (unsigned char)HALO_PORT_NETWORK_VERSION;
	bytes[size++] = (unsigned char)flags;
	put_long(bytes + size, ++lobby.sequence);
	size += 4;
	put_long(bytes + size, (unsigned long)time(NULL));
	size += 4;
	memcpy(bytes + size, p2p_signing_key(), P2P_KEY_SIZE);
	size += P2P_KEY_SIZE;
	memcpy(bytes + size, lobby.token, P2P_TOKEN_SIZE);
	size += P2P_TOKEN_SIZE;
	bytes[size++] = (unsigned char)(lobby.player_count > 255 ? 255 : lobby.player_count);
	bytes[size++] = (unsigned char)(lobby.maximum_player_count > 255 ? 255 : lobby.maximum_player_count);
	bytes[size++] = (unsigned char)lobby.engine_type;
	size += put_text(bytes + size, lobby.name, P2P_LISTING_NAME_SIZE);
	size += put_text(bytes + size, lobby.map, P2P_LISTING_MAP_SIZE);
	size += put_text(bytes + size, lobby.gametype, P2P_LISTING_GAMETYPE_SIZE);
	memset(bytes + size, 0, STAMP_SIZE);
	size += STAMP_SIZE;
	memcpy(data, signature_label, sizeof(signature_label) - 1);
	memcpy(data + sizeof(signature_label) - 1, signed_part, (size_t)size);
	p2p_sign(data, (int)(sizeof(signature_label) - 1) + size, bytes + size);
	return size + P2P_SIGNATURE_SIZE;
}

static int get_text(const unsigned char *bytes, int size, int *offset, char *text, int maximum)
{
	int length;

	if (*offset >= size)
		return 0;
	length = bytes[(*offset)++];
	if (length > maximum || *offset + length > size)
		return 0;
	sanitize(text, maximum, (const char *)bytes + *offset, length);
	*offset += length;
	return 1;
}

/* reads a listing (not its signature); 0 if it is malformed */
static int listing_read(const unsigned char *bytes, int size, struct listing *listing)
{
	int offset = 0;

	if (size < MINIMUM_LISTING_SIZE || size > MAXIMUM_LISTING_SIZE || bytes[0] != 'H' || bytes[1] != 'L' ||
		bytes[2] != LISTING_FORMAT)
	{
		return 0;
	}
	offset = 3;
	listing->version = bytes[offset] << 8 | bytes[offset + 1];
	offset += 2;
	listing->flags = bytes[offset++];
	listing->sequence = get_long(bytes + offset);
	offset += 4;
	listing->time = get_long(bytes + offset);
	offset += 4;
	memcpy(listing->key, bytes + offset, P2P_KEY_SIZE);
	offset += P2P_KEY_SIZE;
	memcpy(listing->token, bytes + offset, P2P_TOKEN_SIZE);
	offset += P2P_TOKEN_SIZE;
	listing->player_count = bytes[offset++];
	listing->maximum_player_count = bytes[offset++];
	listing->engine_type = bytes[offset++];
	if (!get_text(bytes, size, &offset, listing->name, P2P_LISTING_NAME_SIZE) ||
		!get_text(bytes, size, &offset, listing->map, P2P_LISTING_MAP_SIZE) ||
		!get_text(bytes, size, &offset, listing->gametype, P2P_LISTING_GAMETYPE_SIZE))
	{
		return 0;
	}
	offset += STAMP_SIZE;
	if (offset + P2P_SIGNATURE_SIZE != size)
		return 0;
	listing->signed_size = offset;
	return 1;
}

static int listing_signed(const unsigned char *bytes, const struct listing *listing)
{
	unsigned char data[sizeof(signature_label) - 1 + MAXIMUM_LISTING_SIZE];

	memcpy(data, signature_label, sizeof(signature_label) - 1);
	memcpy(data + sizeof(signature_label) - 1, bytes, (size_t)listing->signed_size);
	return p2p_ed25519_verify(listing->key, data, (int)(sizeof(signature_label) - 1) + listing->signed_size,
		bytes + listing->signed_size);
}

/* ---------- hosting */

static void publish(void)
{
	lobby.listing_size = listing_make(lobby.listing, lobby.flags);
	lobby.published_time = p2p_now();
	lobby.republish_wanted = 0;
	p2p_signal_lobby_publish(lobby.listing, lobby.listing_size, 0);
}

/* a publish soon: at once if none was lately, else when TRIGGER_INTERVAL
has passed (plus jitter) */
static void republish_soon(unsigned long jitter)
{
	unsigned long earliest = lobby.published_time + TRIGGER_INTERVAL;
	unsigned long when = p2p_now() + jitter;

	if ((long)(earliest - when) > 0)
		when = earliest;
	if (!lobby.republish_wanted || (long)(lobby.republish_time - when) > 0)
		lobby.republish_time = when;
	lobby.republish_wanted = 1;
}

static void stop_listing(void)
{
	unsigned char tombstone[MAXIMUM_LISTING_SIZE];
	int size = listing_make(tombstone, _listing_closed);

	lobby.listed = 0;
	lobby.republish_wanted = 0;
	p2p_signal_lobby_publish(tombstone, size, 1);
	p2p_signal_lobby_topics(0, lobby.browsing);
	platform_log("Internet play: the game is no longer listed in the server browser");
}

static void update_hosting(const unsigned char *token, int player_count, int maximum_player_count)
{
	int want = token && lobby.public && config_boolean("network.public_lobby");

	if (lobby.listed && (!want || memcmp(token, lobby.token, P2P_TOKEN_SIZE)))
		stop_listing();
	if (!want)
		return;
	if (player_count != lobby.player_count || maximum_player_count != lobby.maximum_player_count)
	{
		lobby.player_count = player_count;
		lobby.maximum_player_count = maximum_player_count;
		if (lobby.listed)
			republish_soon(0);
	}
	if (!lobby.listed)
	{
		memcpy(lobby.token, token, P2P_TOKEN_SIZE);
		lobby.listed = 1;
		p2p_signal_lobby_topics(1, lobby.browsing);
		publish();
		platform_log("Internet play: the game is listed in everyone's server browser (public)");
		return;
	}
	if ((lobby.republish_wanted && (long)(p2p_now() - lobby.republish_time) >= 0) ||
		elapsed(lobby.published_time, REPUBLISH_INTERVAL))
	{
		publish();
	}
}

void p2p_lobby_query_heard(void)
{
	if (lobby.listed)
	{
		unsigned short jitter;

		posix_random_bytes(&jitter, sizeof(jitter));
		republish_soon(jitter % QUERY_JITTER);
	}
}

/* the host's own slot was heard: anything but its listing is mended */
static void own_slot_heard(const unsigned char *payload, int size, int retained)
{
	struct listing listing;

	if (!lobby.listed || (size == lobby.listing_size && !memcmp(payload, lobby.listing, (size_t)size)))
		return;
	/* (an older listing of its own, forwarded live, is the echo of a publish
	before the last; as the slot's retained copy, the slot holds it) */
	if (!retained && listing_read(payload, size, &listing) && !memcmp(listing.key, p2p_signing_key(), P2P_KEY_SIZE) &&
		listing.sequence < lobby.sequence)
	{
		return;
	}
	republish_soon(0);
}

/* ---------- browsing */

static struct game *find_game(const unsigned char *key_hash)
{
	int index;

	for (index = 0; index < MAXIMUM_GAMES; index++)
	{
		if (lobby.games[index].used && !memcmp(lobby.games[index].key_hash, key_hash, P2P_KEY_HASH_SIZE))
			return &lobby.games[index];
	}
	return NULL;
}

static struct tombstone *find_tombstone(const unsigned char *key_hash)
{
	int index;

	for (index = 0; index < MAXIMUM_TOMBSTONES; index++)
	{
		struct tombstone *tombstone = &lobby.tombstones[index];

		if (tombstone->time && !elapsed(tombstone->time, TOMBSTONE_TIME) &&
			!memcmp(tombstone->key_hash, key_hash, P2P_KEY_HASH_SIZE))
		{
			return tombstone;
		}
	}
	return NULL;
}

static int identifier_failed(const unsigned char *identifier)
{
	int index;

	for (index = 0; index < lobby.failed_count; index++)
	{
		if (!memcmp(lobby.failed[index], identifier, P2P_IDENTIFIER_SIZE))
			return 1;
	}
	return 0;
}

void p2p_lobby_slot_heard(const char *hash_text, const unsigned char *payload, int size, int retained)
{
	unsigned char key_hash[P2P_KEY_HASH_SIZE];
	struct game *game;
	struct queued *queued;
	int index;

	if (strlen(hash_text) != 2 * P2P_KEY_HASH_SIZE || strspn(hash_text, "0123456789abcdef") != 2 * P2P_KEY_HASH_SIZE)
		return;
	for (index = 0; index < P2P_KEY_HASH_SIZE; index++)
	{
		unsigned int byte;

		sscanf(hash_text + 2 * index, "%2x", &byte);
		key_hash[index] = (unsigned char)byte;
	}
	{
		unsigned char own[P2P_KEY_HASH_SIZE];

		p2p_key_hash(p2p_public_key(), own);
		if (!memcmp(own, key_hash, P2P_KEY_HASH_SIZE))
			own_slot_heard(payload, size, retained);
	}
	/* (an emptied slot is no news: a wipe is not a delete) */
	if (!lobby.browsing || size < MINIMUM_LISTING_SIZE || size > MAXIMUM_LISTING_SIZE)
		return;
	/* the same listing again: heard, no work */
	game = find_game(key_hash);
	if (game && game->payload_size == size && !memcmp(game->payload, payload, (size_t)size))
	{
		game->heard_time = p2p_now();
		return;
	}
	for (index = 0; index < lobby.queue_count; index++)
	{
		if (lobby.queue[index].size == size && !memcmp(lobby.queue[index].payload, payload, (size_t)size))
			return;
	}
	if (lobby.queue_count == MAXIMUM_QUEUED)
		return;
	queued = &lobby.queue[lobby.queue_count++];
	memcpy(queued->key_hash, key_hash, P2P_KEY_HASH_SIZE);
	memcpy(queued->payload, payload, (size_t)size);
	queued->size = size;
	queued->retained = retained;
}

/* a queued listing, its signature checked: taken or not */
static void listing_take(const struct queued *queued, const struct listing *listing)
{
	struct tombstone *tombstone = find_tombstone(queued->key_hash);
	struct game *game = find_game(queued->key_hash);
	struct p2p_listing *shown;
	unsigned char bytes[P2P_KEY_HASH_SIZE + P2P_TOKEN_SIZE];
	char text[2 * (P2P_KEY_HASH_SIZE + P2P_TOKEN_SIZE) + 1];
	int index;

	if (tombstone && listing->sequence <= tombstone->sequence)
		return;
	if (game && listing->sequence <= game->sequence)
		return;
	if (listing->flags & _listing_closed)
	{
		if (!tombstone)
		{
			tombstone = &lobby.tombstones[lobby.next_tombstone];
			lobby.next_tombstone = (lobby.next_tombstone + 1) % MAXIMUM_TOMBSTONES;
			memcpy(tombstone->key_hash, queued->key_hash, P2P_KEY_HASH_SIZE);
		}
		tombstone->sequence = listing->sequence;
		tombstone->time = p2p_now() ? p2p_now() : 1;
		if (game)
			game->used = 0;
		return;
	}
	if (!game)
	{
		/* the room of one gone longest, if there is none free */
		for (index = 0; index < MAXIMUM_GAMES && lobby.games[index].used; index++)
			;
		if (index == MAXIMUM_GAMES)
		{
			int oldest = 0;

			for (index = 1; index < MAXIMUM_GAMES; index++)
			{
				if ((long)(lobby.games[index].heard_time - lobby.games[oldest].heard_time) < 0)
					oldest = index;
			}
			index = oldest;
		}
		game = &lobby.games[index];
		memset(game, 0, sizeof(*game));
		memcpy(game->key_hash, queued->key_hash, P2P_KEY_HASH_SIZE);
		game->used = 1;
	}
	game->sequence = listing->sequence;
	game->heard_time = p2p_now();
	memcpy(game->payload, queued->payload, (size_t)queued->size);
	game->payload_size = queued->size;
	shown = &game->listing;
	memcpy(bytes, queued->key_hash, P2P_KEY_HASH_SIZE);
	memcpy(bytes + P2P_KEY_HASH_SIZE, listing->token, P2P_TOKEN_SIZE);
	p2p_hex(bytes, sizeof(bytes), text);
	snprintf(shown->invite, sizeof(shown->invite), "halo://join/%s", text);
	p2p_identifier_from_hash(queued->key_hash, shown->identifier);
	memcpy(shown->name, listing->name, sizeof(shown->name));
	memcpy(shown->map, listing->map, sizeof(shown->map));
	memcpy(shown->gametype, listing->gametype, sizeof(shown->gametype));
	shown->player_count = (unsigned char)listing->player_count;
	shown->maximum_player_count = (unsigned char)listing->maximum_player_count;
	shown->engine_type = (unsigned char)listing->engine_type;
	shown->open = (listing->flags & _listing_open) != 0;
	shown->in_progress = (listing->flags & _listing_in_progress) != 0;
	shown->has_teams = (listing->flags & _listing_has_teams) != 0;
	shown->ping = -1;
}

static void update_browsing(void)
{
	unsigned long start = p2p_now();
	int index;

	if (lobby.query_wanted)
	{
		lobby.query_wanted = 0;
		p2p_signal_lobby_query();
	}
	/* the queue, within the budget (one at least) */
	while (lobby.queue_count && (p2p_now() == start || !elapsed(start, VERIFY_BUDGET)))
	{
		struct queued queued = lobby.queue[0];
		struct listing listing;
		unsigned char key_hash[P2P_KEY_HASH_SIZE];
		int good;

		memmove(lobby.queue, lobby.queue + 1, sizeof(*lobby.queue) * (size_t)(--lobby.queue_count));
		if (!listing_read(queued.payload, queued.size, &listing) || listing.version != HALO_PORT_NETWORK_VERSION ||
			!signing_key_hash(listing.key, key_hash) || memcmp(key_hash, queued.key_hash, P2P_KEY_HASH_SIZE))
		{
			continue;
		}
		/* a slot's retained copy: only one of about now (a host that died
		left it, and nothing cleared it) */
		if (queued.retained)
		{
			long difference = (long)(listing.time - (unsigned long)time(NULL));

			if (difference > RETAINED_WINDOW || difference < -RETAINED_WINDOW)
				continue;
		}
		/* (the work, without the lock: the game's threads need not wait) */
		pthread_mutex_unlock(&p2p_lock);
		good = listing_signed(queued.payload, &listing);
		pthread_mutex_lock(&p2p_lock);
		if (good && lobby.browsing)
			listing_take(&queued, &listing);
	}
	if (lobby.queue_count && !lobby.browsing)
		lobby.queue_count = 0;
	for (index = 0; index < MAXIMUM_GAMES; index++)
	{
		if (lobby.games[index].used && elapsed(lobby.games[index].heard_time, GAME_EXPIRY))
			lobby.games[index].used = 0;
	}
}

/* ---------- p2p.c's side */

void p2p_lobby_update(const unsigned char *token, int player_count, int maximum_player_count)
{
	update_hosting(token, player_count, maximum_player_count);
	update_browsing();
}

int p2p_lobby_listed(void)
{
	return lobby.listed;
}

void p2p_lobby_quit(void)
{
	if (!lobby.listed)
		return;
	stop_listing();
	/* (the game is ending: written now, not on the coming passes) */
	p2p_signal_lobby_quit();
}

/* ---------- the game's side */

void p2p_set_hosting_public(int public)
{
	pthread_mutex_lock(&p2p_lock);
	/* (a new invite going private: p2p.c) */
	if (lobby.public && !public)
		p2p_new_invite_if_listed();
	lobby.public = public ? 1 : 0;
	pthread_mutex_unlock(&p2p_lock);
}

void p2p_set_game_listing(const char *name, const char *map, const char *gametype, int engine_type, int open,
	int in_progress, int has_teams)
{
	char new_name[P2P_LISTING_NAME_SIZE + 1];
	char new_map[P2P_LISTING_MAP_SIZE + 1];
	char new_gametype[P2P_LISTING_GAMETYPE_SIZE + 1];
	int flags = (open ? _listing_open : 0) | (in_progress ? _listing_in_progress : 0) |
		(has_teams ? _listing_has_teams : 0);

	sanitize(new_name, P2P_LISTING_NAME_SIZE, name ? name : lobby.name, P2P_LISTING_NAME_SIZE);
	sanitize(new_map, P2P_LISTING_MAP_SIZE, map ? map : lobby.map, P2P_LISTING_MAP_SIZE);
	sanitize(new_gametype, P2P_LISTING_GAMETYPE_SIZE, gametype ? gametype : lobby.gametype,
		P2P_LISTING_GAMETYPE_SIZE);
	/* (only the game's server writes these: unchanged, it need not wait for
	the lock) */
	if (!strcmp(new_name, lobby.name) && !strcmp(new_map, lobby.map) && !strcmp(new_gametype, lobby.gametype) &&
		engine_type == lobby.engine_type && flags == lobby.flags)
	{
		return;
	}
	pthread_mutex_lock(&p2p_lock);
	memcpy(lobby.name, new_name, sizeof(lobby.name));
	memcpy(lobby.map, new_map, sizeof(lobby.map));
	memcpy(lobby.gametype, new_gametype, sizeof(lobby.gametype));
	lobby.engine_type = engine_type;
	lobby.flags = flags;
	if (lobby.listed)
		republish_soon(0);
	pthread_mutex_unlock(&p2p_lock);
}

void p2p_lobby_browse(int on)
{
	pthread_mutex_lock(&p2p_lock);
	on = on && config_boolean("network.public_lobby") ? 1 : 0;
	if (on != lobby.browsing)
	{
		lobby.browsing = on;
		if (!on)
		{
			memset(lobby.games, 0, sizeof(lobby.games));
			lobby.queue_count = 0;
		}
		lobby.query_wanted = on;
		p2p_signal_lobby_topics(lobby.listed, on);
	}
	pthread_mutex_unlock(&p2p_lock);
}

void p2p_lobby_refresh(void)
{
	pthread_mutex_lock(&p2p_lock);
	if (lobby.browsing)
		lobby.query_wanted = 1;
	pthread_mutex_unlock(&p2p_lock);
}

int p2p_lobby_browsing(void)
{
	return lobby.browsing;
}

/* the order shown: the most players first; then those joining has not
failed, the open ones, and by name */
static int listing_order(const void *first, const void *second)
{
	const struct p2p_listing *a = first, *b = second;

	if (a->player_count != b->player_count)
		return b->player_count - a->player_count;
	if (a->failed != b->failed)
		return a->failed - b->failed;
	if (a->open != b->open)
		return b->open - a->open;
	return strcmp(a->name, b->name);
}

int p2p_lobby_games(struct p2p_listing *games, int maximum_count)
{
	int count = 0;
	int index;

	pthread_mutex_lock(&p2p_lock);
	for (index = 0; index < MAXIMUM_GAMES && count < maximum_count; index++)
	{
		if (!lobby.games[index].used)
			continue;
		games[count] = lobby.games[index].listing;
		games[count].failed = (unsigned char)identifier_failed(games[count].identifier);
		count++;
	}
	pthread_mutex_unlock(&p2p_lock);
	qsort(games, (size_t)count, sizeof(*games), listing_order);
	return count;
}

void p2p_lobby_mark_failed(const unsigned char *identifier)
{
	pthread_mutex_lock(&p2p_lock);
	if (!identifier_failed(identifier) && lobby.failed_count < MAXIMUM_GAMES)
		memcpy(lobby.failed[lobby.failed_count++], identifier, P2P_IDENTIFIER_SIZE);
	pthread_mutex_unlock(&p2p_lock);
}
