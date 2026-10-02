/*
CHEATS.H

header included in hcex build.
*/

#ifndef __CHEATS_H
#define __CHEATS_H
#pragma once

/* ---------- constants */

/* port: the pause menu's cheats (ui_widget.c), in its order, toggles first */
enum
{
	_cheat_menu_item_god_mode,
	_cheat_menu_item_infinite_ammo,
	_cheat_menu_item_no_reload,
	_cheat_menu_item_super_jump,
	_cheat_menu_item_active_camouflage,
	_cheat_menu_item_all_weapons,
	NUMBER_OF_CHEAT_MENU_ITEMS,

	NUMBER_OF_CHEAT_MENU_TOGGLES = _cheat_menu_item_active_camouflage,
	MAXIMUM_CHEAT_MENU_LABEL_LENGTH = 31
};

/* ---------- macros */

/* ---------- structures */

struct cheat_globals
{
	boolean deathless_player;
	boolean jetpack;
	boolean infinite_ammo;
	boolean bump_possession;
	boolean super_jump;
	boolean reflexive_damage_effects;
	boolean medusa;
	boolean omnipotent;
	boolean controller_enabled;
	boolean bottomless_clip;
};

/* ---------- prototypes/CHEATS.C */

void cheats_initialize(
	void);
void cheats_initialize_for_new_map(
	void);
void cheat_teleport_to_camera(
	void);
void cheat_active_camouflage(
	void);
void cheat_all_weapons(
	void);
void cheat_all_powerups(
	void);
void cheat_all_vehicles(
	void);
void cheats_dispose(
	void);
void cheats_dispose_from_old_map(
	void);
void cheats_update(
	void);
void cheats_network_client_enforce(
	void);
void cheats_load(
	void);
void cheat_active_camouflage_local_player(
	short player_index);
boolean cheat_menu_available(
	void);
void cheat_menu_item_get_label(
	short item,
	char label[MAXIMUM_CHEAT_MENU_LABEL_LENGTH+1]);
boolean cheat_menu_item_select(
	short item);

/* ---------- globals */

extern struct cheat_globals cheat;

/* ---------- public code */

#endif // __CHEATS_H
