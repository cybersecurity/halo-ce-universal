/*
P2P_DISCORD.C

Discord invites for internet play (p2p.c), through the Discord desktop
client's local RPC socket (a Unix socket, or a named pipe on Windows):
while hosting, the game's activity carries a private party whose join
secret is the invite, so the host can send it with Discord's "Invite to
Join" and whoever accepts it joins. Discord only hands the secret to people
the host invited (asking to join goes unanswered).

It goes through the Discord application in discord.application_id
(config.toml; the project's by default), whose name is what Discord shows
as being played. Without one, or without Discord running, invites are still
links (p2p.c).

The protocol: frames of a little-endian opcode and length, then JSON. The
handshake (opcode 0) names the application; commands and events are opcode
1; 2 closes; 3 and 4 are ping and pong. Nothing here waits for Discord (the
p2p thread holds its lock, which the game's threads take): what it does not
take at once waits in a buffer, and a client that lets that fill up is
disconnected.
*/

#include "platform.h"
#include "posix.h"
#include "port_config.h"
#include "p2p_internal.h"

#include <stdio.h>
#ifdef HALO_64BIT
#include <stdlib.h>
#endif
#include <string.h>

enum
{
	RETRY_INTERVAL = 20000,
	BUFFER_SIZE = 16384,
	MAXIMUM_SECRET_SIZE = 128,

	_opcode_handshake = 0,
	_opcode_frame,
	_opcode_close,
	_opcode_ping,
	_opcode_pong,
};

static struct
{
	int checked;
	int enabled;
	char application[32];
	int handle;
	int ready;
	unsigned long attempt_time;
	int attempted;
	unsigned long nonce;
	unsigned char input[BUFFER_SIZE];
	int input_size;
	unsigned char output[BUFFER_SIZE];
	int output_size;

	/* what to show; changed marks it for sending */
	int hosting;
	char secret[MAXIMUM_SECRET_SIZE];
	int player_count;
	int maximum_player_count;
	int changed;

	/* the Discord user signed in to the client (its READY), as told: an id
	of digits, a name of the letters, digits and marks Discord's allow (a
	host logs them, for a player it drops for cheating) */
	char user_id[P2P_DISCORD_ID_SIZE];
	char user_name[P2P_DISCORD_NAME_SIZE];
} discord = { .handle = -1 };

/* the text kept of a Discord user's id or name: of the characters allowed
(the rest left out), no longer than the size (and ended) */
void p2p_discord_sanitize(char *destination, int size, const char *source, int name)
{
	int length = 0;

	for (; source && *source && length < size - 1; source++)
	{
		char character = *source;

		if ((character >= '0' && character <= '9') ||
			(name && ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
				character == '_' || character == '.' || character == '-')))
		{
			destination[length++] = character;
		}
	}
	destination[length] = 0;
}

/* the Discord user signed in, as told (empty if none); under p2p_lock */
void p2p_discord_user(char *id, int id_size, char *name, int name_size)
{
	p2p_discord_sanitize(id, id_size, discord.user_id, 0);
	p2p_discord_sanitize(name, name_size, discord.user_name, 1);
}

static void discord_close(void)
{
	if (discord.handle >= 0)
		posix_discord_close(discord.handle);
	discord.handle = -1;
	discord.ready = 0;
	discord.input_size = 0;
	discord.output_size = 0;
}

/* what can be written now */
static void discord_flush(void)
{
	while (discord.handle >= 0 && discord.output_size > 0)
	{
		int written = posix_discord_write(discord.handle, discord.output, discord.output_size);

		if (written < 0)
		{
			discord_close();
			return;
		}
		if (!written)
			return;
		memmove(discord.output, discord.output + written, (size_t)(discord.output_size - written));
		discord.output_size -= written;
	}
}

static void discord_send(int opcode, const char *json, int size)
{
	unsigned char *frame;

	if (discord.handle < 0 || size > 2048)
		return;
	if (discord.output_size + 8 + size > BUFFER_SIZE)
	{
		platform_log("Internet play: Discord is not responding; disconnected from it");
		discord_close();
		return;
	}
	frame = discord.output + discord.output_size;
	frame[0] = (unsigned char)opcode;
	frame[1] = frame[2] = frame[3] = 0;
	frame[4] = (unsigned char)size;
	frame[5] = (unsigned char)(size >> 8);
	frame[6] = frame[7] = 0;
	memcpy(frame + 8, json, (size_t)size);
	discord.output_size += 8 + size;
	discord_flush();
}

static void send_activity(void)
{
	char json[1024];
	char party[2 * P2P_IDENTIFIER_SIZE + 1];
	int size;

	p2p_hex(p2p_identifier(), P2P_IDENTIFIER_SIZE, party);
	if (discord.hosting)
	{
		size = snprintf(json, sizeof(json),
			"{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"%lu\",\"args\":{\"pid\":%lu,\"activity\":{"
			"\"details\":\"Hosting a game\",\"state\":\"Invite only\","
			/* the image uploaded as "logo" under the application's Rich
			Presence art assets */
			"\"assets\":{\"large_image\":\"logo\",\"large_text\":\"Halo: Combat Evolved\"},"
			"\"party\":{\"id\":\"%s\",\"size\":[%d,%d]},"
			"\"secrets\":{\"join\":\"%s\"},\"instance\":false}}}",
			++discord.nonce, (unsigned long)posix_process_id(), party, discord.player_count,
			discord.maximum_player_count, discord.secret);
	}
	else
	{
		size = snprintf(json, sizeof(json),
#ifdef HALO_64BIT
			"{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"%lu\",\"args\":{\"pid\":%lu,\"activity\":{"
			"\"details\":\"In Menus\","
			"\"assets\":{\"large_image\":\"logo\",\"large_text\":\"Halo: Combat Evolved\"},"
			"\"instance\":false}}}",
#else
			"{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"%lu\",\"args\":{\"pid\":%lu}}",
#endif
			++discord.nonce, (unsigned long)posix_process_id());
	}
	discord_send(_opcode_frame, json, size);
}

/* a string field's value in json, without its escapes' backslashes */
static int json_string(const char *json, const char *name, char *value, int size)
{
	char pattern[64];
	const char *start;
	int length = 0;

	snprintf(pattern, sizeof(pattern), "\"%s\"", name);
	start = strstr(json, pattern);
	if (!start)
		return 0;
	start += strlen(pattern);
	while (*start == ' ' || *start == ':')
		start++;
	if (*start++ != '"')
		return 0;
	while (*start && *start != '"' && length < size - 1)
	{
		if (*start == '\\' && start[1])
			start++;
		value[length++] = *start++;
	}
	value[length] = 0;
	return 1;
}

static void frame_received(int opcode, char *json)
{
	char event[64];

	switch (opcode)
	{
	case _opcode_frame:
		if (!json_string(json, "evt", event, sizeof(event)))
			break;
		if (!strcmp(event, "READY"))
		{
			char command[256];
			int size;

			discord.ready = 1;
			platform_log("Internet play: connected to Discord");
			/* (who is signed in: its user, after the configuration) */
			{
				const char *user = strstr(json, "\"user\"");
				char value[128];

				discord.user_id[0] = 0;
				discord.user_name[0] = 0;
				if (user && json_string(user, "id", value, sizeof(value)))
					p2p_discord_sanitize(discord.user_id, sizeof(discord.user_id), value, 0);
				if (user && json_string(user, "username", value, sizeof(value)))
					p2p_discord_sanitize(discord.user_name, sizeof(discord.user_name), value, 1);
			}
			size = snprintf(command, sizeof(command),
				"{\"cmd\":\"SUBSCRIBE\",\"evt\":\"ACTIVITY_JOIN\",\"nonce\":\"%lu\"}", ++discord.nonce);
			discord_send(_opcode_frame, command, size);
			discord.changed = 1;
		}
		else if (!strcmp(event, "ACTIVITY_JOIN"))
		{
			char secret[MAXIMUM_SECRET_SIZE];

			if (json_string(json, "secret", secret, sizeof(secret)))
			{
				platform_log("Internet play: accepted a Discord invite");
				p2p_invite_received(secret);
			}
		}
		else if (!strcmp(event, "ERROR"))
		{
			char message[256];

			if (json_string(json, "message", message, sizeof(message)))
				platform_log("Internet play: Discord: %s", message);
		}
		break;
	case _opcode_close:
		discord_close();
		break;
	case _opcode_ping:
		discord_send(_opcode_pong, json, (int)strlen(json));
		break;
	}
}

static void discord_read(void)
{
	for (;;)
	{
		int size = posix_discord_read(discord.handle, discord.input + discord.input_size,
			BUFFER_SIZE - 1 - discord.input_size);

		if (size < 0)
		{
			if (discord.ready)
				platform_log("Internet play: disconnected from Discord");
			discord_close();
			return;
		}
		if (size == 0)
			break;
		discord.input_size += size;
		if (discord.input_size >= BUFFER_SIZE - 1)
			break;
	}
	while (discord.input_size >= 8)
	{
		int opcode = discord.input[0] | discord.input[1] << 8;
		int length = discord.input[4] | discord.input[5] << 8 | discord.input[6] << 16;
		char json[BUFFER_SIZE];

		if (length > BUFFER_SIZE - 9)
		{
			discord_close();
			return;
		}
		if (discord.input_size < 8 + length)
			break;
		memcpy(json, discord.input + 8, (size_t)length);
		json[length] = 0;
		memmove(discord.input, discord.input + 8 + length, (size_t)(discord.input_size - 8 - length));
		discord.input_size -= 8 + length;
		frame_received(opcode, json);
		if (discord.handle < 0)
			return;
	}
}

void p2p_discord_update(void)
{
	if (!discord.checked)
	{
		const char *application = config_string("discord.application_id");

		discord.checked = 1;
		if (*application && strlen(application) < sizeof(discord.application))
		{
			char scheme[48];

			strcpy(discord.application, application);
			discord.enabled = 1;
			/* how Discord starts the game for an invite when it is not
			running; the game then receives the invite once connected */
			snprintf(scheme, sizeof(scheme), "discord-%s", application);
			/* (it lets go of the p2p lock while it may wait) */
			p2p_register_url_scheme(scheme, "Halo: Combat Evolved");
#ifdef HALO_64BIT
#ifdef __APPLE__
			/* Discord on macOS opens discord-<application>:// (Info.plist
			declares it) unless a games/<application>.json names a command,
			which it takes for a page of its own instead of running it: the
			one an earlier version wrote goes */
			{
				const char *home = getenv("HOME");

				if (home && *home)
				{
					char path[1024];

					snprintf(path, sizeof(path), "%s/Library/Application Support/discord/games/%s.json",
						home, application);
					remove(path);
				}
			}
#endif
#endif
		}
	}
	if (!discord.enabled)
		return;
	if (discord.handle < 0)
	{
		char handshake[128];
		int size;

		/* (unsigned, as the clock wraps) */
		if (discord.attempted && (unsigned int)(p2p_now() - discord.attempt_time) < (unsigned int)RETRY_INTERVAL)
			return;
		discord.attempted = 1;
		discord.attempt_time = p2p_now();
		discord.handle = posix_discord_connect();
		if (discord.handle < 0)
			return;
		size = snprintf(handshake, sizeof(handshake), "{\"v\":1,\"client_id\":\"%s\"}", discord.application);
		discord_send(_opcode_handshake, handshake, size);
		if (discord.handle < 0)
			return;
	}
	discord_flush();
	if (discord.handle < 0)
		return;
	discord_read();
	if (discord.ready && discord.changed)
	{
		discord.changed = 0;
		send_activity();
	}
}

void p2p_discord_set_hosting(const char *secret, int player_count, int maximum_player_count)
{
	discord.hosting = secret != NULL;
	if (secret)
		snprintf(discord.secret, sizeof(discord.secret), "%s", secret);
	discord.player_count = player_count;
	discord.maximum_player_count = maximum_player_count;
	discord.changed = 1;
}
