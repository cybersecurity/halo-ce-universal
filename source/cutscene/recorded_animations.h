/*
RECORDED_ANIMATIONS.H

header included in hcex build.
*/

#ifndef __RECORDED_ANIMATIONS_H
#define __RECORDED_ANIMATIONS_H
#pragma once

/* ---------- constants */

/* ---------- macros */

/* ---------- structures */

/* ---------- prototypes/RECORDED_ANIMATIONS.C */

void recorded_animations_initialize(
	void);
void recorded_animations_dispose(
	void);
void recorded_animations_dispose_from_old_map(
	void);
void recorded_animations_initialize_for_new_map(
	void);
void recorded_animations_update(
	void);
void recorded_animations_clear_debug_storage(
	void);

boolean recorded_animation_controlling_unit(
	long unit_index);
void recorded_animation_kill(
	long unit_index);
long recorded_animation_get_time_left(
	long unit_index);

boolean recorded_animation_play(
	long unit_index,
	short animation_index);
boolean recorded_animation_play_and_delete(
	long unit_index,
	short animation_index);
boolean recorded_animation_play_and_hover(
	long unit_index,
	short animation_index);
void render_debug_recording(
	void);

/* ---------- globals */

/* ---------- public code */

#ifdef HALO_64BIT
void render_debug_recording(
	void);

#endif
#endif // __RECORDED_ANIMATIONS_H
