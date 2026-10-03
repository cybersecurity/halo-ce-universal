/*
NETWORK_GAME_MANAGER.C

symbols in this file:
0011A170 0040:
	_network_game_invalidate_player (0000)
0011A1B0 0090:
	_network_game_add_machine (0000)
0011A240 0090:
	_network_game_update_machine (0000)
0011A2D0 0070:
	_xbox_set_machine_name (0000)
0011A340 0090:
	_network_game_generate_local_machine_name (0000)
0011A3D0 0040:
	_network_game_end_and_load_ui (0000)
0011A410 00b0:
	_network_game_reset_for_next_round (0000)
0011A4C0 0010:
	_network_game_assign_players_to_team (0000)
0011A4D0 0030:
	_network_player_is_valid (0000)
0011A500 00b0:
	_network_game_invalidate_machine (0000)
0011A5B0 01a0:
	_network_game_add_player (0000)
0011A750 0140:
	_compare_network_players (0000)
0011A890 0080:
	_network_game_spawn_player (0000)
0011A910 0090:
	_network_game_player_is_valid (0000)
0011A9A0 00a0:
	_network_game_invalidate (0000)
0011AA40 00a0:
	_network_game_update_player (0000)
0011AAE0 00c0:
	_network_game_remove_player (0000)
0011ABA0 01b0:
	_network_game_create_game_objects (0000)
0011AD50 00e0:
	_network_game_remove_machine (0000)
00283DA4 0031:
	??_C@_0DB@DPKLAPFE@c?3?2halo?2SOURCE?2networking?2networ@ (0000)
00283DD8 0035:
	??_C@_0DF@FMJAGNKK@game?5?$CG?$CG?5machine?5?$CG?$CG?5network_machi@ (0000)
00283E10 0043:
	??_C@_0ED@LDCLPLHI@?8?$CFs?8?5is?5not?5a?5valid?5machine?5name@ (0000)
00283E54 0016:
	??_C@_0BG@OIMNPPEI@XSetNickname?$CI?$CJ?5failed?$AA@ (0000)
00283E6C 002d:
	??_C@_0CN@JCIPKPMA@XSetNickname?$CI?$CJ?5failed?5to?5set?5sys@ (0000)
00283E9C 001c:
	??_C@_0BM@GOCBNICP@system?5nickname?5set?5to?5?8?$CFs?8?$AA@ (0000)
00283EB8 0036:
	??_C@_0DG@KPIAMNFF@game?5?$CG?$CG?5?$CImachine_index?$DMMAXIMUM_N@ (0000)
00283EF0 0039:
	??_C@_0DJ@PKEDFKIN@game?5is?5already?5at?5maximum?5playe@ (0000)
00283F2C 000f:
	??_C@_0P@KHIDKAOH@game?5?$CG?$CG?5player?$AA@ (0000)
00283F40 004b:
	??_C@_0EL@LDBNCIG@multiple?5players?5on?5the?5same?5mac@ (0000)
00283F8C 000f:
	??_C@_0P@NHMFPDJJ@player?5?$CG?$CG?5game?$AA@ (0000)
00283F9C 002c:
	??_C@_0CM@KIIMJJBI@tried?5to?5update?5a?5player?5with?5in@ (0000)
00283FC8 002c:
	??_C@_0CM@NFHLBLAN@tried?5to?5remove?5a?5player?5with?5in@ (0000)
00283FF4 0017:
	??_C@_0BH@GFBBGLI@?$CB?$CCbad?5game?5connection?$CC?$AA@ (0000)
0028400C 0024:
	??_C@_0CE@GKPEJODL@failed?5to?5remove?5a?5machine?8s?5pla@ (0000)
00284030 0010:
	??_C@_0BA@OPIINEDG@game?5?$CG?$CG?5machine?$AA@ (0000)
*/

/* ---------- headers */

#include "cseries/cseries.h"
#include "cseries/errors.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "interface/player_ui.h"
#include "saved games/player_profile.h"
#include "main/main.h"
#include "memory/data.h"
#include "network_game_globals.h"
#include "network_game_manager.h"
#include "network_game_ui.h"
#include "networking/network_server_manager.h"
#include "objects/objects.h"
#include "units/units.h"
#include "text/unicode.h"

#include <xtl.h>

/* ---------- constants */

/* the machine and player slots of a network game: the Xbox's 4 and 16, or the
native builds' session limits (port/linux/include/halo_port_limits.h) */
#define NETWORK_GAME_MACHINE_SLOTS HALO_PORT_MAXIMUM_NETWORK_MACHINES
#define NETWORK_GAME_PLAYER_SLOTS HALO_PORT_MAXIMUM_NETWORK_PLAYERS

/* port: the distributed netcode names players by their datums' absolute
indices (port/linux/game), which every machine must share: a machine that
joined the game in progress too, which has neither the players who left
(their datums stay until the game ends) nor the order the others added
players in. So there each player's datum is its slot in the host's player
list: every machine makes it there (network_game_spawn_player), and the host
gives a player added to the game in progress a slot whose datum is free.
(Only in a game: in the lobby the host runs the user interface's scenario,
whose local player holds datum 0, and a full lobby could not use slot 0.)
A player who quit the game holds his slot no longer once his unit is gone:
his datum gives way to the next player there (network_game_spawn_player),
so that those who quit do not keep the game from filling. Nor in the lobby
between games, where the last game's datums stay until the next is made. */
/* a player who quit the game and has no unit left: his slot can be another's */
static boolean network_game_player_slot_reusable(
	struct player_datum const *player)
{
	return player->quit_out_of_game && player->unit_index == NONE;
}

static boolean network_game_player_slot_held(
	long slot)
{
	struct network_game_server *server = global_network_game_server_get();
	struct player_datum *player;

	if (!game_in_progress() || !game_engine_running() ||
		!player_data || !player_data->valid || slot >= player_data->maximum_count ||
		(server && !network_game_server_playing(server)))
	{
		return FALSE;
	}
	player = (struct player_datum *)((byte *)player_data->data + player_data->size * slot);
	if (((struct datum_header *)player)->identifier == 0)
		return FALSE;
	return !network_game_player_slot_reusable(player);
}

enum
{
	MAXIMUM_NETWORK_MACHINE_COUNT = NETWORK_GAME_MACHINE_SLOTS,
};

/* ---------- macros */

#define network_machine_is_valid(machine) \
	((machine) && (machine)->machine_index >= 0 && (machine)->machine_index < NETWORK_GAME_MACHINE_SLOTS)

/* ---------- structures */

struct game_options
{
	unsigned long flags;
	short code_version;
	short difficulty;
	unsigned long random_seed;
	char map_name[256];
};

typedef char network_game_players_offset_assert[
	offsetof(struct network_game, players) == HALO_PORT_NETWORK_GAME_PLAYERS_OFFSET ? 1 : -1];
typedef char network_game_size_assert[
	sizeof(struct network_game) == HALO_PORT_NETWORK_GAME_SIZE ? 1 : -1];

/* ---------- prototypes */

/* ---------- globals */

/* ---------- public code */

boolean network_game_add_machine(
	struct network_game *game,
	struct network_machine *machine)
{
	long machine_index;
	boolean result = FALSE;

	match_assert(
		"c:\\halo\\SOURCE\\networking\\network_game_manager.c",
		0x6A,
		game && machine && network_machine_is_valid(machine));

	/* port: at its own index, not the first free slot: the host numbers a
	machine by its connection's slot, which must be its slot here (two
	machines once shared an index) */
	machine_index = machine->machine_index;
	if (!network_machine_is_valid(&game->machines[machine_index]))
	{
		csmemcpy(&game->machines[machine_index], machine, sizeof(*machine));
		game->machine_count++;
		result = TRUE;
	}

	return result;
}

boolean network_game_update_machine(
	struct network_game *game,
	struct network_machine *machine)
{
	long machine_index;
	boolean result = FALSE;

	match_assert(
		"c:\\halo\\SOURCE\\networking\\network_game_manager.c",
		0x81,
		game && machine && network_machine_is_valid(machine));

	for (machine_index = 0; machine_index < NETWORK_GAME_MACHINE_SLOTS; machine_index++)
	{
		if (game->machines[machine_index].machine_index == machine->machine_index)
		{
			csmemcpy(&game->machines[machine_index], machine, sizeof(*machine));
			result = TRUE;
			break;
		}
	}

	return result;
}

void xbox_set_machine_name(
	char const *machine_name)
{
	wchar_t wide_machine_name[32];

	if (machine_name && machine_name[0])
	{
		if (ascii_to_wide(machine_name, wide_machine_name, sizeof(wide_machine_name)))
		{
			wide_machine_name[31] = 0;
			if (!XSetNicknameW(wide_machine_name, TRUE))
				error(2, "XSetNickname() failed");
		}
		else
		{
			error(2, "'%s' is not a valid machine name (max. name length= %d characters)", wide_machine_name, 31);
		}
	}

	return;
}

void network_game_generate_local_machine_name(
	wchar_t *machine_name)
{
	char ascii_machine_name[32];
	HANDLE find_handle;

	/* port: a machine that brings one player to the game (the one who
	joined multiplayer on it) is named after that player's profile, not the
	system's random nickname: the system link list shows a host's game by it */
	{
		short local_player_index;
		short joined_player_index = NONE;
		short joined_player_count = 0;

		for (local_player_index = 0; local_player_index < MAXIMUM_LOCAL_PLAYERS; local_player_index++)
		{
			if (player_ui_local_player_wants_to_play_multiplayer(local_player_index))
			{
				joined_player_index = local_player_index;
				joined_player_count++;
			}
		}
		if (joined_player_count == 1)
		{
			struct player_profile profile;

			player_ui_get_active_player_profile(joined_player_index, &profile);
			if (profile.player_name[0])
			{
				ustrncpy(machine_name, profile.player_name, MAXIMUM_PLAYER_PROFILE_NAME_LENGTH);
				machine_name[MIN(MAXIMUM_PLAYER_PROFILE_NAME_LENGTH, 32) - 1] = 0;
				return;
			}
		}
	}
	find_handle = XFindFirstNicknameW(FALSE, machine_name, 32);

	if (find_handle == INVALID_HANDLE_VALUE)
	{
		ustrncpy(machine_name, network_game_get_random_player_name(), 32);
		machine_name[31] = 0;
		if (XSetNicknameW(machine_name, TRUE))
			error(2, "system nickname set to '%s'", wide_to_ascii(machine_name, ascii_machine_name, 32));
		else
			error(2, "XSetNickname() failed to set system nickname");
	}
	else
	{
		XFindClose(find_handle);
	}

	machine_name[31] = 0;
	return;
}

void network_game_invalidate_player(
	struct network_player *player)
{
	match_assert(
		"c:\\halo\\SOURCE\\networking\\network_game_manager.c",
		0x58,
		player);

	player->machine_index = NONE;
	player->controller_index = NONE;
	player->team_index = NONE;
	player->player_list_index = NONE;
	player->name[0] = 0;

	return;
}

void network_game_end_and_load_ui(
	struct network_game *game)
{
	if (game->local_data.game_objects_loaded)
		main_load_ui_scenario(TRUE);

	csmemset(&game->local_data, 0, sizeof(game->local_data));

	return;
}

void network_game_assign_players_to_team(
	struct network_game *game,
	char const *prefix)
{
	return;
}

boolean network_game_add_player(
	struct network_game *game,
	struct network_player *player)
{
	long player_index;
	long new_player_index;
	boolean result = FALSE;

	match_assert(
		"c:\\halo\\SOURCE\\networking\\network_game_manager.c",
		0xBB,
		game && player);

	if (game->player_count < game->maximum_players)
	{
		if (VALID_INDEX(player->machine_index, MAXIMUM_NETWORK_MACHINE_COUNT) &&
			VALID_INDEX(player->controller_index, MAXIMUM_LOCAL_PLAYERS))
		{
			for (player_index = 0; player_index < NETWORK_GAME_PLAYER_SLOTS; player_index++)
			{
				if (game->players[player_index].machine_index == player->machine_index &&
					game->players[player_index].controller_index == player->controller_index)
				{
					break;
				}
			}

			if (player_index == NETWORK_GAME_PLAYER_SLOTS && network_player_is_valid(player))
			{
				new_player_index = NONE;
				/* (the host's slot, as it chose it) */
				if (player->player_list_index != NONE)
				{
					if (VALID_INDEX(player->player_list_index, NETWORK_GAME_PLAYER_SLOTS) &&
						game->players[player->player_list_index].player_list_index == NONE)
					{
						new_player_index = player->player_list_index;
					}
				}
				else
				for (player_index = 0; player_index < NETWORK_GAME_PLAYER_SLOTS; player_index++)
				{
					if (game->players[player_index].player_list_index == NONE
						/* (not the slot of a player who left the game in progress) */
						&& !network_game_player_slot_held(player_index)
						)
					{
						new_player_index = player_index;
						break;
					}
				}

				if ((player->player_list_index == NONE || new_player_index == player->player_list_index) &&
					new_player_index != NONE)
				{
					player->player_list_index = (char)new_player_index;
					csmemcpy(&game->players[new_player_index], player, sizeof(struct network_player));
					game->player_count++;
					result = TRUE;
				}
			}
		}
	}
	else
	{
		error(2, "game is already at maximum players; can't add new player");
	}

	return result;
}

/* port: whether a player can be added to the game (a free slot, which in a
game in progress is not the slot of a player who left: network_game_add_player) */
boolean network_game_has_free_player_slot(
	struct network_game *game)
{
	long player_index;

	if (game->player_count >= game->maximum_players)
		return FALSE;
	for (player_index = 0; player_index < NETWORK_GAME_PLAYER_SLOTS; player_index++)
	{
		if (game->players[player_index].player_list_index == NONE &&
			!network_game_player_slot_held(player_index))
		{
			return TRUE;
		}
	}
	return FALSE;
}

boolean network_player_is_valid(
	struct network_player *player)
{
	if (player &&
		player->controller_index >= 0 &&
		player->controller_index < MAXIMUM_LOCAL_PLAYERS &&
		player->machine_index >= 0 &&
		/* the Xbox game checks the machine against the split screen limit,
		which only works while both are 4 */
		(long)player->machine_index < MAXIMUM_NETWORK_MACHINE_COUNT)
	{
		return TRUE;
	}

	return FALSE;
}

void network_game_invalidate_machine(
	struct network_game *game,
	word machine_index)
{
	long player_index;

	match_assert(
		"c:\\halo\\SOURCE\\networking\\network_game_manager.c",
		0x40,
		game && (machine_index<MAXIMUM_NETWORK_MACHINE_COUNT));

	game->machines[machine_index].machine_index = NONE;
	game->machines[machine_index].name[0] = 0;

	for (player_index = 0; player_index < NETWORK_GAME_PLAYER_SLOTS; player_index++)
	{
		if (game->players[player_index].machine_index == machine_index)
			network_game_invalidate_player(&game->players[player_index]);
	}

	return;
}

boolean network_game_spawn_player(
	struct network_player *player)
{
	long player_index;
	short controller_index;

	match_assert(
		"c:\\halo\\SOURCE\\networking\\network_game_manager.c",
		0x1BC,
		network_player_is_valid(player));

	controller_index = network_game_player_is_local(player) ? player->controller_index : NONE;
	/* (the datum at the player's slot, with the identifier datum_new would
	give it) */
	if (VALID_INDEX(player->player_list_index, NETWORK_GAME_PLAYER_SLOTS))
	{
		/* port: a player who quit there gives way (network_game_player_slot_held),
		and his units forget him */
		if (player_data && player_data->valid && player->player_list_index < player_data->maximum_count)
		{
			struct player_datum *quitter = (struct player_datum *)((byte *)player_data->data +
				player_data->size * player->player_list_index);

			/* (the host's choice stands: a client whose clock has not yet
			come to the quit of the player there gives way too, to another
			machine's player) */
			if (((struct datum_header *)quitter)->identifier != 0 &&
				(network_game_player_slot_reusable(quitter) ||
					quitter->network_player_data.machine_index != player->machine_index ||
					quitter->network_player_data.controller_index != player->controller_index))
			{
				long quitter_index = ((long)(word)((struct datum_header *)quitter)->identifier << 16) |
					player->player_list_index;
				struct object_iterator iterator;

				object_iterator_new(&iterator, _object_mask_unit, 0);
				while (object_iterator_next(&iterator))
				{
					struct unit_datum *unit = unit_get(iterator.index);

					if (unit->unit.player_index == quitter_index)
						unit->unit.player_index = NONE;
				}
				player_delete(quitter_index);
			}
		}
		player_index = player_new(player->machine_index,
			((long)(word)player_data->next_identifier << 16) | player->player_list_index, controller_index, player);
	}
	else
	player_index = player_new(player->machine_index, NONE, controller_index, player);
	if (player_index != NONE)
	{
		player->player_list_index = (char)player_index;
		return TRUE;
	}

	return FALSE;
}

boolean network_game_player_is_valid(
	struct network_player *player,
	struct network_game *game)
{
	long player_index;
	struct network_player *current_player;
	boolean result = FALSE;

	match_assert(
		"c:\\halo\\SOURCE\\networking\\network_game_manager.c",
		0x247,
		player && game);

	if (network_player_is_valid(player))
	{
		current_player = &game->players[0];
		for (player_index = 0; player_index < NETWORK_GAME_PLAYER_SLOTS; player_index++, current_player++)
		{
			if (current_player->machine_index == player->machine_index &&
				current_player->controller_index == player->controller_index)
			{
				result = TRUE;
				break;
			}
		}
	}

	return result;
}

void network_game_reset_for_next_round(
	struct network_game *game,
	boolean unload_game_objects)
{
	match_assert(
		"c:\\halo\\SOURCE\\networking\\network_game_manager.c",
		0x22D,
		game);

	if (unload_game_objects && game->local_data.game_objects_loaded)
	{
		main_load_ui_scenario(TRUE);
		csmemset(&game->local_data, 0, sizeof(game->local_data));
		if (global_network_game_server_get())
			game_connection_set(2);
		else if (global_network_game_client_get())
			game_connection_set(1);
	}
	else
	{
		csmemset(&game->local_data, 0, sizeof(game->local_data));
	}

	game_time_end();
	return;
}

void network_game_invalidate(
	struct network_game *game)
{
	long machine_index;

	match_assert(
		"c:\\halo\\SOURCE\\networking\\network_game_manager.c",
		0x23,
		game);

	csmemset(game, 0, sizeof(*game));
	csmemset(&game->map, 0, sizeof(game->map));
	game->machine_count = 0;
	game->player_count = 0;

	for (machine_index = 0; machine_index < NETWORK_GAME_MACHINE_SLOTS; machine_index++)
		network_game_invalidate_machine(game, (short)machine_index);

	csmemset(game->players, NONE, sizeof(game->players));
	game->minimum_players = 2;
	/* (port: 128 in the native builds, halo_port_limits.h) */
	game->maximum_players = NETWORK_GAME_PLAYER_SLOTS;
	game->local_data.game_objects_loaded = FALSE;

	return;
}

boolean network_game_update_player(
	struct network_game *game,
	struct network_player *player)
{
	struct network_player *current_player;
	boolean result = FALSE;

	match_assert(
		"c:\\halo\\SOURCE\\networking\\network_game_manager.c",
		0x101,
		game && player);

	/* (port: the slot comes from the wire: one out of the list wrote over
	the machines) */
	if (network_game_player_is_valid(player, game) &&
		VALID_INDEX(player->player_list_index, NETWORK_GAME_PLAYER_SLOTS))
	{
		current_player = &game->players[player->player_list_index];
		if (current_player->controller_index == player->controller_index &&
			current_player->machine_index == player->machine_index)
		{
			csmemcpy(current_player, player, sizeof(*current_player));
			result = TRUE;
		}
	}

	if (!result)
		error(2, "tried to update a player with indvalid data");

	return result;
}

boolean network_game_remove_player(
	struct network_game *game,
	struct network_player *player)
{
	long player_index;
	boolean result = FALSE;

	match_assert(
		"c:\\halo\\SOURCE\\networking\\network_game_manager.c",
		0x120,
		game && player);

	if (network_game_player_is_valid(player, game))
	{
		for (player_index = 0; player_index < NETWORK_GAME_PLAYER_SLOTS; player_index++)
		{
			if (network_player_is_valid(&game->players[player_index]) &&
				game->players[player_index].machine_index == player->machine_index &&
				game->players[player_index].controller_index == player->controller_index)
			{
				network_game_invalidate_player(&game->players[player_index]);
				game->player_count--;
				result = TRUE;
				break;
			}
		}
	}
	else
	{
		error(2, "tried to remove a player with indvalid data");
	}

	return result;
}

boolean network_game_remove_machine(
	struct network_game *game,
	struct network_machine *machine)
{
	long machine_index;
	long player_index;
	boolean result = FALSE;

	match_assert(
		"c:\\halo\\SOURCE\\networking\\network_game_manager.c",
		0x97,
		game && machine);

	if (network_machine_is_valid(machine))
	{
		for (machine_index = 0; machine_index < NETWORK_GAME_MACHINE_SLOTS; machine_index++)
		{
			if (game->machines[machine_index].machine_index == machine->machine_index)
			{
				for (player_index = 0; player_index < NETWORK_GAME_PLAYER_SLOTS; player_index++)
				{
					if (network_player_is_valid(&game->players[player_index]) &&
						game->players[player_index].machine_index == machine->machine_index)
					{
						if (!network_game_remove_player(game, &game->players[player_index]))
							error(2, "failed to remove a machine's player");
					}
				}

				network_game_invalidate_machine(game, machine->machine_index);
				game->machine_count--;
				result = TRUE;
				break;
			}
		}
	}

	return result;
}

boolean network_game_create_game_objects(
	struct network_game *game)
{
	long player_index;
	struct game_options options;

	match_assert(
		"c:\\halo\\SOURCE\\networking\\network_game_manager.c",
		0x170,
		game);

	game_options_new(&options);
	csstrncpy(options.map_name, game->map.name, sizeof(game->map.name) - 1);
	options.difficulty = game->difficulty;

	switch (game_connection())
	{
		case _game_connection_network_client:
		case _game_connection_network_server:
			options.random_seed = network_game_get_random_seed();
			break;

		case _game_connection_film_playback:
			options.random_seed = game->random_seed;
			break;

		default:
			match_assert(
				"c:\\halo\\SOURCE\\networking\\network_game_manager.c",
				0x17F,
				!"bad game connection");
			break;
	}

	game_precache_new_map(options.map_name, TRUE);
	main_menu_unload();

	/* port: and whenever a map is loaded. After a game the client is back in
	the main menu's map with its clock ended (network_game_dispose_game_objects),
	so not in progress: the next game's map was loaded over it, and the
	texture cache, opened again, lost the menu's loaded textures (a crash in
	lruv_block_new) */
	if (game_in_progress() || game_map_loaded())
	{
		game_dispose_from_old_map();
		game_unload();
	}

	if (game->variant.game_engine_index)
	{
		game_set_game_variant(&game->variant);
		/* port: and its PC options */
		game_set_game_variant_options(&game->variant_options);
	}

	if (game_load(&options))
	{
		game->local_data.game_objects_loaded = TRUE;
		game_initialize_for_new_map();

		/* (the players stay in their slots, which are their datums:
		network_game_spawn_player) */
		for (player_index = 0; player_index < NETWORK_GAME_PLAYER_SLOTS; player_index++)
		{
			if (!network_player_is_valid(&game->players[player_index]))
			{
				/* (the slots of players who left are among them) */
				continue;
			}
			game->players[player_index].player_list_index = (char)player_index;

			if (!network_game_spawn_player(&game->players[player_index]))
			{
				game->local_data.game_objects_loaded = FALSE;
				break;
			}
		}
	}
	else
	{
		error(0, "game_load() failed.");
	}

	return game->local_data.game_objects_loaded;
}

/* ---------- private code */
