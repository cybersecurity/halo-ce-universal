/*
NETWORK_GAME_UI.H

header included in hcex build.
*/

#ifndef __NETWORK_GAME_UI_H
#define __NETWORK_GAME_UI_H
#pragma once

/* ---------- constants */

/* ---------- macros */

/* ---------- structures */

/* ---------- prototypes/EXAMPLE.C */

wchar_t const *network_game_get_random_player_name(
	void);

#ifdef HALO_WEB
boolean network_game_player_name_is_blank(wchar_t const *name);
#endif

/* ---------- globals */

/* ---------- public code */

#endif // __NETWORK_GAME_UI_H
