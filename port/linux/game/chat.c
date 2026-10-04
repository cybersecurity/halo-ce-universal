/*
CHAT.C

Text chat in network games. T opens a line to say something to everyone, Y
one to the player's team (to everyone in a game without teams), typed on the
console's line (terminal.c: the two are never open at once); enter sends it,
escape (or enter on nothing) closes it. While it is open the keyboard does not
play (xinput_sdl.c), as for the console.

What is said goes to the host, which names who said it from its own players
and passes it on: to every machine, or to those with a player of the team
(network_distributed.c). Each shows it on the HUD of its players it is for,
as "Name: text", in lines of the HUD's length.
*/

#include "cseries.h"
#include "cseries/errors.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "input/input.h"
#include "interface/hud_messaging.h"
#include "interface/terminal.h"
#include "interface/ui_widget.h"
#include "main/console.h"
#include "memory/data.h"
#include "text/unicode.h"

#include "chat.h"

#include <string.h>

/* cseries_windows.c's */
unsigned long system_milliseconds(void);

/* ---------- constants */

enum
{
	/* the HUD's message lines: their text, its end included (hud_messaging.c's) */
	HUD_LINE_SIZE = 63,
	/* the most of them one message takes */
	CHAT_MAXIMUM_LINES = 3,
	/* a player's name, as players.h has it */
	CHAT_NAME_LENGTH = 12,
};

/* ---------- globals */

static struct
{
	boolean active;
	boolean team_only;
	/* T's and Y's last frame, so that a press opens the line once */
	boolean open_keys_down[2];
	/* when this machine last said something (0: not yet) */
	unsigned long last_sent_time;
	struct terminal_gets_state input_state;
} chat_globals;

static real_argb_color const chat_color = {1.f, 1.f, 1.f, 1.f};
static real_argb_color const chat_team_color = {1.f, 0.5f, 0.8f, 1.f};

/* ---------- private code */

/* in a network game, playing (not its scores after, nor a menu up) */
static boolean chat_available(
	void)
{
	short connection = game_connection();

	return game_in_progress() &&
		(connection == _game_connection_network_client || connection == _game_connection_network_server) &&
		!game_engine_showing_postgame() &&
		!ui_widgets_active();
}

static boolean chat_can_open(
	void)
{
	return !chat_globals.active && chat_available() && !console_is_active() && !terminal_gets_active();
}

static void chat_open(
	boolean team_only)
{
	csmemset(&chat_globals.input_state, 0, sizeof(chat_globals.input_state));
	chat_globals.team_only = team_only;
	chat_globals.input_state.color = team_only ? chat_team_color : chat_color;
	csstrcpy(chat_globals.input_state.prompt, team_only ? "Team: " : "All: ");
	if (terminal_gets_begin(&chat_globals.input_state))
	{
		/* (no longer than a message: terminal_gets_begin allows a line) */
		chat_globals.input_state.edit.maximum_length = CHAT_MAXIMUM_TEXT_SIZE - 1;
		chat_globals.active = TRUE;
	}
}

static void chat_close(
	void)
{
	if (chat_globals.active)
	{
		terminal_gets_end(&chat_globals.input_state);
		chat_globals.active = FALSE;
	}
}

static void chat_send(
	void)
{
	char text[CHAT_MAXIMUM_TEXT_SIZE];
	unsigned long now = system_milliseconds();

	chat_text_clean(text, chat_globals.input_state.result, sizeof(text));
	if (!text[0])
		return;
	/* (the host refuses one too: dropped without a word) */
	if (chat_text_has_link(text))
		return;
	/* (no more than one in CHAT_COOLDOWN_MILLISECONDS, as the host keeps
	to: the player told how long is left, not their message lost) */
	if (chat_globals.last_sent_time && now - chat_globals.last_sent_time < CHAT_COOLDOWN_MILLISECONDS)
	{
		wchar_t wait[HUD_LINE_SIZE];
		long seconds = (long)((CHAT_COOLDOWN_MILLISECONDS - (now - chat_globals.last_sent_time) + 999) / 1000);

		usprintf(wait, seconds == 1 ? L"You can chat again in %ld second" : L"You can chat again in %ld seconds",
			seconds);
		hud_print_message(0, wait);
		return;
	}
	chat_globals.last_sent_time = now ? now : 1;
	/* (the keyboard's player: the first) */
	distributed_chat_send(0, chat_globals.team_only, text);
}

/* the text lowercased, with the ways of writing a dot that hide a link made
dots ("(.)", "[dot]", " dot ", ...), and no spaces about a dot */
static void chat_link_normalize(
	char const *text,
	char *normalized,
	long size)
{
	static char const *const dots[] =
	{
		"(.)", "[.]", "{.}", "<.>", "(dot)", "[dot]", "{dot}", "<dot>", " dot ",
	};
	long length = 0;
	long index = 0;

	while (text[index] && length < size - 1)
	{
		boolean dot = text[index] == '.';
		long dot_length = 1;
		short dot_index;

		for (dot_index = 0; !dot && dot_index < (short)NUMBEROF(dots); dot_index++)
		{
			long pattern_length = csstrlen(dots[dot_index]);
			long character;

			for (character = 0; character < pattern_length; character++)
			{
				char letter = text[index + character];

				if (letter >= 'A' && letter <= 'Z')
					letter += 'a' - 'A';
				if (letter != dots[dot_index][character])
					break;
			}
			if (character == pattern_length)
			{
				dot = TRUE;
				dot_length = pattern_length;
			}
		}
		if (dot)
		{
			/* (no spaces before it, nor after) */
			while (length > 0 && normalized[length - 1] == ' ')
				length--;
			normalized[length++] = '.';
			index += dot_length;
			while (text[index] == ' ')
				index++;
		}
		else
		{
			char letter = text[index++];

			normalized[length++] = letter >= 'A' && letter <= 'Z' ? letter + 'a' - 'A' : letter;
		}
	}
	normalized[length] = 0;
}

static boolean chat_link_letter(
	char letter)
{
	return letter >= 'a' && letter <= 'z';
}

static boolean chat_link_digit(
	char letter)
{
	return letter >= '0' && letter <= '9';
}

/* ---------- public code */

boolean chat_text_has_link(
	char const *text)
{
	/* the endings of web sites' names that links have (not every one: those
	that are words, as "is" and "so", would refuse sentences) */
	static char const *const endings[] =
	{
		"com", "net", "org", "gg", "io", "xyz", "co", "tv", "ly", "me", "info", "biz", "site", "online",
		"link", "app", "dev", "shop", "store", "club", "live", "fun", "top", "cc", "tk", "ml", "ga", "cf",
		"gq", "pw", "ws", "ru", "uk", "de", "fr", "us", "ca", "au", "cn", "nl", "br", "pl", "edu", "gov",
	};
	char normalized[CHAT_MAXIMUM_TEXT_SIZE * 2];
	long index;

	chat_link_normalize(text, normalized, sizeof(normalized));
	if (strstr(normalized, "http") || strstr(normalized, "www.") || strstr(normalized, "://"))
		return TRUE;

	for (index = 0; normalized[index]; index++)
	{
		long start;
		long end;
		short ending_index;

		if (normalized[index] != '.' || index == 0)
			continue;

		/* an IP address: four numbers of up to three digits, with dots */
		{
			long position = index;
			short numbers = 0;

			while (position > 0 && chat_link_digit(normalized[position - 1]))
				position--;
			if (position < index && index - position <= 3)
			{
				numbers = 1;
				while (numbers < 4 && normalized[position] && normalized[position] != ' ')
				{
					while (chat_link_digit(normalized[position]))
						position++;
					if (normalized[position] != '.' || !chat_link_digit(normalized[position + 1]))
						break;
					position++;
					numbers++;
				}
				if (numbers == 4)
					return TRUE;
			}
		}

		/* a name, a dot, and one of the endings, not followed by more of the word */
		if (!chat_link_letter(normalized[index - 1]) && !chat_link_digit(normalized[index - 1]) &&
			normalized[index - 1] != '-')
		{
			continue;
		}
		start = index + 1;
		for (end = start; chat_link_letter(normalized[end]); end++)
			;
		if (end == start || chat_link_digit(normalized[end]))
			continue;
		for (ending_index = 0; ending_index < (short)NUMBEROF(endings); ending_index++)
		{
			if ((long)csstrlen(endings[ending_index]) == end - start &&
				!strncmp(&normalized[start], endings[ending_index], end - start))
			{
				return TRUE;
			}
		}
	}
	return FALSE;
}

boolean chat_is_active(
	void)
{
	/* (closed as the game ends or a menu comes up: chat_update runs only
	in a game, and this every frame) */
	if (chat_globals.active && !chat_available())
		chat_close();
	return chat_globals.active;
}

boolean chat_key_passes(
	int virtual_key)
{
	return chat_is_active() || ((virtual_key == 'T' || virtual_key == 'Y') && chat_can_open());
}

boolean chat_update(
	void)
{
	boolean keys_down[2];
	short key_index;

	if (chat_is_active())
	{
		for (key_index = 0; chat_globals.active && key_index < chat_globals.input_state.key_count; key_index++)
		{
			switch (chat_globals.input_state.keys[key_index].key_code)
			{
			case _key_escape:
				chat_close();
				break;
			case _key_return:
			case _keypad_enter:
				chat_send();
				chat_close();
				break;
			default:
				break;
			}
		}
	}

	keys_down[0] = input_key_is_down(_key_t) != 0;
	keys_down[1] = input_key_is_down(_key_y) != 0;
	if (chat_can_open())
	{
		if (keys_down[0] && !chat_globals.open_keys_down[0])
			chat_open(FALSE);
		else if (keys_down[1] && !chat_globals.open_keys_down[1])
			chat_open(game_engine_has_teams());
	}
	chat_globals.open_keys_down[0] = keys_down[0];
	chat_globals.open_keys_down[1] = keys_down[1];

	return chat_globals.active;
}

void chat_text_clean(
	char *destination,
	char const *source,
	long size)
{
	long length = 0;
	long index;

	for (index = 0; source[index] && length < size - 1; index++)
	{
		char character = source[index];

		if (character < 32 || character > 126)
			continue;
		/* (no spaces before the text) */
		if (character == ' ' && length == 0)
			continue;
		destination[length++] = character;
	}
	/* (nor after it) */
	while (length > 0 && destination[length - 1] == ' ')
		length--;
	destination[length] = 0;
}

void chat_show(
	wchar_t const *name,
	long team_index,
	boolean team_only,
	char const *text)
{
	wchar_t message[CHAT_NAME_LENGTH + 16 + CHAT_MAXIMUM_TEXT_SIZE];
	wchar_t lines[CHAT_MAXIMUM_LINES][HUD_LINE_SIZE];
	char log_name[CHAT_NAME_LENGTH + 1];
	short line_count = 0;
	long name_length = ustrnlen(name, CHAT_NAME_LENGTH);
	long length = 0;
	long start;
	long index;
	short local_player_index;

	/* "(Team) Name: text", the name printable */
	if (team_only)
	{
		ustrcpy(message, L"(Team) ");
		length = ustrlen(message);
	}
	for (index = 0; index < name_length; index++)
	{
		wchar_t character = name[index];

		message[length++] = character < 32 ? L'?' : character;
		log_name[index] = character >= 32 && character < 127 ? (char)character : '?';
	}
	log_name[name_length] = 0;
	message[length++] = L':';
	message[length++] = L' ';
	for (index = 0; text[index] && length < (long)NUMBEROF(message) - 1; index++)
		message[length++] = (wchar_t)(unsigned char)text[index];
	message[length] = 0;

	/* in lines of the HUD's length, broken at a space where there is one */
	for (start = 0; start < length && line_count < CHAT_MAXIMUM_LINES; line_count++)
	{
		long end = length;

		while (start < length && message[start] == L' ')
			start++;
		if (end - start > HUD_LINE_SIZE - 1)
		{
			end = start + HUD_LINE_SIZE - 1;
			for (index = end; index > start + (HUD_LINE_SIZE - 1) / 2; index--)
			{
				if (message[index] == L' ')
				{
					end = index;
					break;
				}
			}
		}
		ustrncpy(lines[line_count], &message[start], end - start);
		lines[line_count][end - start] = 0;
		start = end;
	}

	for (local_player_index = 0; local_player_index < MAXIMUM_LOCAL_PLAYERS; local_player_index++)
	{
		long player_index = local_player_get_player_index(local_player_index);
		struct player_datum *player = player_index != NONE ? player_try_and_get(player_index) : NULL;

		if (!player || (team_only && player->team_index != team_index))
			continue;
		for (index = 0; index < line_count; index++)
			hud_print_message(local_player_index, lines[index]);
	}
	error(_error_log, "chat%s: %s: %s", team_only ? " (team)" : "", log_name, text);
}
