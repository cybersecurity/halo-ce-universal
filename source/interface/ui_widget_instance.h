/*
UI_WIDGET_INSTANCE.H

The widget instance, shared by every unit that touches one.

The decompiled units each described widget instances locally, with byte
padding standing in for what they didn't use, sized for January's 32-bit
pointers. On a 64-bit host those copies disagreed about where the sibling
and child links live, so there is one definition. Fields the units named
differently share a union.
*/

#ifndef __UI_WIDGET_INSTANCE_H
#define __UI_WIDGET_INSTANCE_H
#pragma once

#include "cseries/cseries.h"

struct widget_animation_data
{
	short current_frame_index;
	short first_frame_index;
	short last_frame_index;
	short number_of_sprite_frames;
};

#define ui_widget_animation_data widget_animation_data

struct widget_instance
{
	int definition_tag_index;
	char const *name;
	short local_player_index;
	short horizontal_offset;
	short vertical_offset;
	short type;
	boolean visible;
	boolean render_regardless_of_controller_index;
	union { boolean disabled; boolean never_receive_events; };
	boolean pause_game_time;
	boolean delete_recursion_lock;
	union { boolean widget_is_error_dialog; boolean error_dialog; };
	boolean close_if_local_player_controller_present;
	byte pad17;
	int creation_time;
	unsigned int milliseconds_to_auto_close;
	unsigned int auto_close_fade_time;
	real alpha_modifier;
	struct widget_instance *previous;
	struct widget_instance *next;
	struct widget_instance *parent;
	struct widget_instance *child;
	struct widget_instance *focused_child;
	union
	{
		struct
		{
			wchar_t *text;
			short string_list_index;
		} text_box;
		struct
		{
			union { short selected_index; short selected_list_item_index; };
			/* counted back toward zero one step per rendered frame; the two
			tab functions start it at +15 and -15 and the column list renderer
			clears it */
			union { short last_list_tab_direction; short list_item_top_index; };
			void *list_items;
			word number_of_items;
			struct widget_instance *extended_description;
			wchar_t *item_text;
		} list;
		int value;
	} parameters;
	struct widget_animation_data animation;
};

#endif
