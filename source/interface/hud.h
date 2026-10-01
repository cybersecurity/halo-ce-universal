/*
HUD.H

header included in hcex build.
*/

#ifndef __HUD_H
#define __HUD_H
#pragma once

/* ---------- constants */

/* ---------- macros */

/* ---------- structures */

#ifndef HALO_64BIT
struct hud_globals_definition;
#endif
struct tag_block;
union real_argb_color;
union real_point3d;

struct hud_scripted_globals_definition
{
	boolean show_hud;
	boolean show_hud_help_text;
	byte pad[2];
};

/* ---------- prototypes/HUD.C */

void hud_initialize(
	void);
void hud_dispose(
	void);
void hud_dispose_from_old_map(
	void);
boolean scripted_show_hud(
	boolean show);
boolean scripted_show_hud_help_text(
	boolean show);
void hud_update(
	void);
void hud_initialize_for_new_map(
	void);
void hud_initialize_nav_points(
	void);
void hud_initialize_nav_points_for_new_map(
	void);
void hud_dispose_nav_points_from_old_map(
	void);
void hud_dispose_nav_points(
	void);
void hud_activate_team_nav_point_with_flag(
	short nav_index,
	short team_index,
	short flag_index,
	float vertical_offset);
void hud_activate_team_nav_point_with_object(
	short nav_index,
	short team_index,
	long object_index,
	float vertical_offset);
void hud_deactivate_team_nav_point_with_flag(
	short team_index,
	short flag_index);
void hud_deactivate_team_nav_point_with_object(
	short team_index,
	long object_index);
void hud_unit_activate_nav_point_with_flag(
	short nav_index,
	long unit_index,
	short flag_index,
	float vertical_offset);
void hud_unit_activate_nav_point_with_object(
	short nav_index,
	long unit_index,
	long object_index,
	float vertical_offset);
void hud_unit_deactivate_nav_point_with_flag(
	long unit_index,
	short flag_index);
void hud_unit_deactivate_nav_point_with_object(
	long unit_index,
	long object_index);
void hud_update_nav_points(
	void);

wchar_t const *hud_get_item_string(
	long string_index);

void hud_load(
	boolean load);
void hud_autosave(
	boolean active);

void hud_picked_up_powerup(
	short local_player_index,
	long powerup_definition_index);
void hud_picked_up_grenade(
	short local_player_index,
	long grenade_definition_index);
void hud_picked_up_ammunition(
	short local_player_index,
	long weapon_definition_index,
	short ammunition_count);
void hud_picked_up_weapon(
	short local_player_index,
	long weapon_definition_index);
void hud_render_nav_points(
	short local_player_index);
void hud_draw_screen(
	void);

/* ---------- prototypes/HUD_NAV_POINTS.C */

short find_nav_point(
	char const *name);
short hud_get_nav_point_render_type(
	short local_player_index,
	union real_point3d const *head,
	union real_point3d const *position,
	long reference_object_index);
void custom_render_nav_point(
	short local_player_index,
	union real_point3d const *position,
	short nav_index,
	short waypoint_type);

/* ---------- prototypes/HUD_SOUNDS.C */

void hud_play_sound(
	short local_player_index,
	unsigned long state_flags,
	struct tag_block const *sounds,
	long *sound_indices,
	word *played_flags);

/* ---------- globals */

extern struct hud_globals_definition *hud_globals;
extern struct hud_scripted_globals_definition *hud_scripted_globals;

/* ---------- public code */

#endif // __HUD_H
