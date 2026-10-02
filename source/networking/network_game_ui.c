/*
NETWORK_GAME_UI.C

symbols in this file:
0011AE30 0060:
	_network_game_get_random_player_name (0000)
*/

/* ---------- headers */

#include "cseries.h"
#include "math/real_math.h"
#include "network_game_ui.h"
#include "tag_files/tag_groups.h"
#include "text/text_group.h"

/* ---------- constants */

/* ---------- macros */

/* ---------- structures */

/* ---------- prototypes */

/* ---------- globals */

/* ---------- public code */

#ifdef HALO_WEB
boolean network_game_player_name_is_blank(wchar_t const *name)
{
	short index;
	if (!name) return TRUE;
	/* Network player names hold eleven UTF-16 units plus NUL. */
	for (index = 0; index < 11 && name[index]; index++)
	{
		unsigned short character = (unsigned short)name[index];
		if (character > 0x20 && character != 0x85 && character != 0xA0 && character != 0x1680 &&
			!(character >= 0x2000 && character <= 0x200B) &&
			character != 0x2028 && character != 0x2029 && character != 0x202F &&
			character != 0x205F && character != 0x2060 && character != 0x3000 && character != 0xFEFF)
			return FALSE;
	}
	return TRUE;
}
#endif

wchar_t const *network_game_get_random_player_name(
	void)
{
	wchar_t const *player_name = L"";
	long string_list_index = tag_loaded('ustr', "ui\\random_player_names");

	if (string_list_index != NONE)
	{
		struct string_list *string_list = unicode_string_list_definition_get(string_list_index);

		if (string_list
#ifdef HALO_WEB
			&& string_list->strings.count > 0
#endif
			)
		{
			short string_index = seed_random_range(
				get_global_local_random_seed_address(),
				0,
				string_list->strings.count - 1);
			player_name = unicode_string_list_get_string(string_list_index, string_index);
		}
	}

#ifdef HALO_WEB
	/* Late joins happen after the UI map (and its random-name tag) unloads.
	Use local randomness so naming never changes the gameplay RNG sequence. */
	if (network_game_player_name_is_blank(player_name))
	{
		static wchar_t const *const names[] = {
			L"Falcon", L"Raven", L"Viper", L"Nova", L"Echo", L"Ghost", L"Blaze", L"Orbit",
			L"Cobalt", L"Onyx", L"Ember", L"Atlas", L"Lynx", L"Drake", L"Sable", L"Astra",
			L"Bolt", L"Frost", L"Wolf", L"Hawk", L"Noble", L"Dusk", L"Meteor", L"Rogue",
			L"Raptor", L"Flint", L"Kestrel", L"Mantis", L"Quartz", L"Talon", L"Phoenix", L"Cipher"
		};
		player_name = names[seed_random_range(get_global_local_random_seed_address(), 0, NUMBEROF(names) - 1)];
	}
#endif

	return player_name;
}

/* ---------- private code */
