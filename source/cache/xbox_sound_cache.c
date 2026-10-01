/*
XBOX_SOUND_CACHE.C

symbols in this file:
001AD980 0030:
	_sound_cache_delete (0000)
001AD9B0 0010:
	_sound_cache_open (0000)
001AD9C0 0040:
	_sound_cache_idle (0000)
001ADA00 0050:
	_sound_cache_sound_new (0000)
001ADA50 0130:
	_sound_cache_sound_delete (0000)
001ADB80 0070:
	_sound_cache_sound_finished (0000)
001ADBF0 0040:
	_sound_cache_sound_hardware_lock (0000)
001ADC30 0030:
	_sound_cache_sound_hardware_unlock (0000)
001ADC60 0040:
	_sound_cache_locked_block_proc (0000)
001ADCA0 00c0:
	_sound_cache_delete_block_proc (0000)
001ADD60 0040:
	_cache_block_get_sound_permutation_name (0000)
001ADDA0 0130:
	_sound_cache_start_loading_sound (0000)
001ADED0 00b0:
	_sound_cache_new (0000)
001ADF80 0060:
	_sound_cache_flush (0000)
001ADFE0 0060:
	_sound_cache_close (0000)
001AE040 0160:
	__sound_cache_sound_request (0000)
001AE1A0 0100:
	_render_inverse_transform_screen_point (0000)
001AE2A0 0170:
	_sound_cache_debug_render (0000)
002A7818 0028:
	??_C@_0CI@CEOIDJNM@hardware?5sound?5reference?5count?5f@ (0000)
002A7840 0028:
	??_C@_0CI@ECPIFOOO@c?3?2halo?2SOURCE?2cache?2xbox_sound_@ (0000)
002A7868 0020:
	??_C@_0CA@EAEGBHAP@sound?9?$DOcache_base_address?$DN?$DNNULL?$AA@ (0000)
002A7888 0049:
	??_C@_0EJ@CCGMBLDE@tried?5to?5delete?5sound?5?$CFs?$CI?$CFs?$CJ?5fro@ (0000)
002A78D8 0049:
	??_C@_0EJ@LIMGNEKN@tried?5to?5delete?5sound?5?$CFs?$CI?$CFs?$CJ?5fro@ (0000)
002A7924 0026:
	??_C@_0CG@DKDBDOMH@cache_sound?9?$DOsoftware_reference_@ (0000)
002A794C 0011:
	??_C@_0BB@DNICGKNA@?9?9?9?5finish?5?$CFd?5?$CFs?$AA@ (0000)
002A7960 0033:
	??_C@_0DD@CMHIKMHN@cache_sound?9?$DOsound?9?$DOcache_block_@ (0000)
002A7998 0042:
	??_C@_0EC@GIEKOHPI@tried?5to?5delete?5sound?5?$CFs?$CI?$CFs?$CJ?5fro@ (0000)
002A79DC 0008:
	??_C@_07IFMLPBAP@?$CFs?5?$CI?$CFs?$CJ?$AA@ (0000)
002A79E4 000f:
	??_C@_0P@IBINCFI@d?3?2stabbed?4txt?$AA@ (0000)
002A79F8 0046:
	??_C@_0EG@BAHEDCLE@SOUND?5CACHE?5BLOWN?$CB?$CB?$CB?$CB?5double?9cli@ (0000)
002A7A40 0043:
	??_C@_0ED@KGGMCECB@?$CB?$CB?$CB?$CB?$CB?$CB?$CB?$CB?$CB?$CB?$CB?$CB?$CB?$CB?$CB?$CB?$CB?$CB?$CB?$CB?$CB?$CB?$CB?$CB?$CB?$CB?$CB?$CB?$CB?$CB?$CB?$CB@ (0000)
002A7A84 0029:
	??_C@_0CJ@LADKOEPK@new_cache_sound_index?$DN?$DNcache_blo@ (0000)
002A7AB0 0026:
	??_C@_0CG@ICHLCFN@xbox_sound_cache_globals?4base_ad@ (0000)
002A7AD8 001f:
	??_C@_0BP@BPBJCKGO@xbox_sound_cache_globals?4cache?$AA@ (0000)
002A7AF8 0011:
	??_C@_0BB@KEGBILOA@xbox?5sound?5cache?$AA@ (0000)
002A7B0C 0026:
	??_C@_0CG@BBBHJDMH@xbox_sound_cache_globals?4cache_s@ (0000)
002A7B34 000b:
	??_C@_0L@LPLEFDPA@xbox?5sound?$AA@ (0000)
002A7B40 0038:
	??_C@_0DI@BGGMDELJ@cache_sound?9?$DOsoftware_reference_@ (0000)
002A7B78 0012:
	??_C@_0BC@MNBCOGO@?9?9?9?5request?5?$CFd?5?$CFs?$AA@ (0000)
002A7B8C 001a:
	??_C@_0BK@KPEOMHDP@sound?9?$DOcache_tag_index?$CB?$DN0?$AA@ (0000)
002A7BA8 0013:
	??_C@_0BD@CEJPLKLE@load?5?$HM?$HM?5?$CBreference?$AA@ (0000)
002A7BBC 000f:
	??_C@_0P@FHHOOBEP@load?5?$HM?$HM?5?$CBblock?$AA@ (0000)
004D1088 0110:
	_xbox_sound_cache_globals (0000)
*/

/* ---------- headers */

#include "cseries/cseries.h"
#include "cseries/cseries_windows.h"
#include "cseries/errors.h"
#include "cache/cache_files.h"
#include "cache/physical_memory_map.h"
#include "cache/sound_cache.h"
#include "interface/terminal.h"
#include "memory/data.h"
#include "memory/lruv_cache.h"
#include "render/render.h"
#include "render/render_debug.h"
#include "scenario/scenario.h"
#include "sound/sound_definitions.h"
#include "tag_files/tag_files.h"

#include <xtl.h>
#ifdef HALO_64BIT
#include "cache/cache_files.h"
#include "cseries/cseries_windows.h"
#endif

/* ---------- constants */

/* ---------- macros */

#define cache_block_index unknown0
#define cache_base_address unknown1
#define cache_tag_index unknown2
#define runtime_tag_index unknown3

/* ---------- structures */

struct xbox_cache_sound_datum
{
	short identifier;
	boolean loaded;
	boolean initialized;
	byte software_reference_count;
	byte hardware_reference_count;
	byte reserved006[2];
	struct sound_permutation *sound;
};

struct xbox_sound_cache_globals
{
	char debug_block_name[0x100];
	struct data_array *cache_sounds;
	byte *base_address;
	struct lruv_cache *cache;
	unsigned long last_allocation_failure_time;
};

typedef char verify_xbox_cache_sound_loaded_offset[
	offsetof(
		struct xbox_cache_sound_datum,
		loaded) == 0x2 ? 1 : -1];
typedef char verify_xbox_cache_sound_initialized_offset[
	offsetof(
		struct xbox_cache_sound_datum,
		initialized) == 0x3 ? 1 : -1];

typedef char verify_xbox_cache_sound_hardware_reference_count_offset[
	offsetof(
		struct xbox_cache_sound_datum,
		hardware_reference_count) == 0x5 ? 1 : -1];
typedef char verify_xbox_cache_sound_software_reference_count_offset[
	offsetof(
		struct xbox_cache_sound_datum,
		software_reference_count) == 0x4 ? 1 : -1];
typedef char verify_xbox_cache_sound_sound_offset[
	offsetof(
		struct xbox_cache_sound_datum,
		sound) == 0x8 ? 1 : -1];
#ifndef HALO_64BIT
typedef char verify_xbox_cache_sound_datum_size[
	sizeof(struct xbox_cache_sound_datum) == 0xC ? 1 : -1];
#endif
typedef char verify_xbox_sound_cache_sounds_offset[
	offsetof(
		struct xbox_sound_cache_globals,
		cache_sounds) == 0x100 ? 1 : -1];
#ifndef HALO_64BIT
typedef char verify_xbox_sound_cache_base_address_offset[
	offsetof(
		struct xbox_sound_cache_globals,
		base_address) == 0x104 ? 1 : -1];
typedef char verify_xbox_sound_cache_cache_offset[
	offsetof(
		struct xbox_sound_cache_globals,
		cache) == 0x108 ? 1 : -1];
typedef char verify_xbox_sound_cache_last_allocation_failure_time_offset[
	offsetof(
		struct xbox_sound_cache_globals,
		last_allocation_failure_time) == 0x10C ? 1 : -1];
typedef char verify_xbox_sound_cache_globals_size[
	sizeof(struct xbox_sound_cache_globals) == 0x110 ? 1 : -1];

#endif
/* ---------- prototypes */

static long sound_cache_locked_block_proc(
	long block_index);
static void sound_cache_delete_block_proc(
	long block_index);

/* ---------- globals */

short assertion_count;
boolean debug_sound_cache;
boolean debug_sound_reference_counts;
static struct xbox_sound_cache_globals xbox_sound_cache_globals = { 0 };

/* ---------- public code */

void sound_cache_delete(
	void)
{
	data_dispose(xbox_sound_cache_globals.cache_sounds);
	lruv_delete(xbox_sound_cache_globals.cache);
	xbox_sound_cache_globals.base_address = NULL;

	return;
}

void sound_cache_open(
	void)
{
	data_make_valid(xbox_sound_cache_globals.cache_sounds);

	return;
}

void sound_cache_idle(
	void)
{
	lruv_idle(xbox_sound_cache_globals.cache);
	match_vassert(
		"c:\\halo\\SOURCE\\cache\\xbox_sound_cache.c",
		148,
		assertion_count == 0,
		"hardware sound reference count failure.");

	return;
}

void sound_cache_sound_new(
	long cache_tag_index,
	struct sound_permutation *sound)
{
	match_vassert(
		"c:\\halo\\SOURCE\\cache\\xbox_sound_cache.c",
		158,
		sound->cache_base_address == 0,
		"sound->cache_base_address==NULL");
	sound->cache_block_index = NONE;
	sound->cache_base_address = 0;
	sound->cache_tag_index = cache_tag_index;

	return;
}

void sound_cache_sound_delete(
	struct sound_permutation *sound)
{
	if (sound->cache_block_index != NONE)
	{
		match_vassert(
			"c:\\halo\\SOURCE\\cache\\xbox_sound_cache.c",
			173,
			((struct xbox_cache_sound_datum *)datum_get(
				xbox_sound_cache_globals.cache_sounds,
				sound->cache_block_index))->software_reference_count == 0,
			csprintf(
				temporary,
				"tried to delete sound %s(%s) from the cache while it was playing (soft).",
				tag_get_name(((struct xbox_cache_sound_datum *)datum_get(
					xbox_sound_cache_globals.cache_sounds,
					sound->cache_block_index))->sound->runtime_tag_index),
				((struct xbox_cache_sound_datum *)datum_get(
					xbox_sound_cache_globals.cache_sounds,
					sound->cache_block_index))->sound));
		match_vassert(
			"c:\\halo\\SOURCE\\cache\\xbox_sound_cache.c",
			174,
			((struct xbox_cache_sound_datum *)datum_get(
				xbox_sound_cache_globals.cache_sounds,
				sound->cache_block_index))->hardware_reference_count == 0,
			csprintf(
				temporary,
				"tried to delete sound %s(%s) from the cache while it was playing (hard).",
				tag_get_name(((struct xbox_cache_sound_datum *)datum_get(
					xbox_sound_cache_globals.cache_sounds,
					sound->cache_block_index))->sound->runtime_tag_index),
				((struct xbox_cache_sound_datum *)datum_get(
					xbox_sound_cache_globals.cache_sounds,
					sound->cache_block_index))->sound));
		lruv_block_delete(
			xbox_sound_cache_globals.cache,
			sound->cache_block_index);
	}

	sound->cache_block_index = NONE;
	sound->cache_base_address = 0;

	return;
}

void sound_cache_sound_finished(
	struct sound_permutation *sound)
{
	struct xbox_cache_sound_datum *cache_sound;

	cache_sound = datum_get(
		xbox_sound_cache_globals.cache_sounds,
		sound->cache_block_index);
	if (debug_sound_reference_counts)
	{
		error(
			_error_silent,
			"--- finish %d %s",
			cache_sound->software_reference_count,
			cache_sound->sound);
	}
	match_assert(
		"c:\\halo\\SOURCE\\cache\\xbox_sound_cache.c",
		263,
		cache_sound->software_reference_count);
	cache_sound->software_reference_count--;

	return;
}

void sound_cache_sound_hardware_lock(
	struct sound_permutation *sound)
{
	struct xbox_cache_sound_datum *cache_sound;

	cache_sound = datum_get(
		xbox_sound_cache_globals.cache_sounds,
		sound->cache_block_index);
	if (cache_sound->hardware_reference_count < UNSIGNED_CHAR_MAX)
	{
		cache_sound->hardware_reference_count++;
	}
	else
	{
		assertion_count++;
	}

	return;
}

void sound_cache_sound_hardware_unlock(
	struct sound_permutation *sound)
{
	struct xbox_cache_sound_datum *cache_sound = datum_get(
		xbox_sound_cache_globals.cache_sounds,
		sound->cache_block_index);

	if (cache_sound->hardware_reference_count)
	{
		cache_sound->hardware_reference_count--;
	}
	else
	{
		assertion_count++;
	}

	return;
}

void sound_cache_new(
	void)
{
	xbox_sound_cache_globals.cache_sounds = data_new(
		"xbox sound",
		512,
		sizeof(struct xbox_cache_sound_datum));
	match_vassert(
		"c:\\halo\\SOURCE\\cache\\xbox_sound_cache.c",
		69,
		xbox_sound_cache_globals.cache_sounds != NULL,
		"xbox_sound_cache_globals.cache_sounds");
	xbox_sound_cache_globals.cache = lruv_new(
		"xbox sound cache",
		1024,
		12,
		512,
		sound_cache_delete_block_proc,
		(lruv_locked_block_proc)sound_cache_locked_block_proc);
	match_vassert(
		"c:\\halo\\SOURCE\\cache\\xbox_sound_cache.c",
		73,
		xbox_sound_cache_globals.cache != NULL,
		"xbox_sound_cache_globals.cache");
	xbox_sound_cache_globals.base_address =
		physical_memory_get_sound_cache_base_address();
	match_vassert(
		"c:\\halo\\SOURCE\\cache\\xbox_sound_cache.c",
		76,
		xbox_sound_cache_globals.base_address != NULL,
		"xbox_sound_cache_globals.base_address");

	return;
}

void sound_cache_flush(
	void)
{
	struct data_iterator iterator;
	struct xbox_cache_sound_datum *cache_sound;

	data_iterator_new(&iterator, xbox_sound_cache_globals.cache_sounds);
	while ((cache_sound = data_iterator_next(&iterator)) != NULL)
	{
		if (cache_sound->software_reference_count == 0 &&
			cache_sound->hardware_reference_count == 0)
		{
			sound_cache_sound_delete(cache_sound->sound);
		}
	}

	return;
}

void sound_cache_close(
	void)
{
	struct data_iterator iterator;
	struct xbox_cache_sound_datum *cache_sound;

	data_iterator_new(&iterator, xbox_sound_cache_globals.cache_sounds);
	while ((cache_sound = data_iterator_next(&iterator)) != NULL)
	{
		/* port: the map is going, every sound stopped and the platform's
		channels flushed (sound_dispose_from_old_map): a sound still counted
		as playing is a count never given back, not a sound playing (one, a
		weapon's charging loop, halted a client as a game ended); let go,
		not halted on */
		if (cache_sound->software_reference_count != 0 || cache_sound->hardware_reference_count != 0)
		{
			error(
				_error_silent,
				"sound %s still counted as playing (%d, %d) as the map closed; let go",
				cache_sound->sound ? tag_get_name(cache_sound->sound->runtime_tag_index) : "?",
				cache_sound->software_reference_count,
				cache_sound->hardware_reference_count);
			cache_sound->software_reference_count = 0;
			cache_sound->hardware_reference_count = 0;
		}
		sound_cache_sound_delete(cache_sound->sound);
	}
	data_make_invalid(xbox_sound_cache_globals.cache_sounds);

	return;
}

/* ---------- private code */

static long sound_cache_locked_block_proc(
	long block_index)
{
	struct xbox_cache_sound_datum *cache_sound;

	cache_sound = datum_get(xbox_sound_cache_globals.cache_sounds, block_index);
	if (cache_sound->loaded &&
		cache_sound->software_reference_count == 0 &&
		cache_sound->hardware_reference_count == 0)
	{
		return FALSE;
	}

	return TRUE;
}

static void sound_cache_delete_block_proc(
	long block_index)
{
	struct xbox_cache_sound_datum *cache_sound;

	cache_sound = datum_get(xbox_sound_cache_globals.cache_sounds, block_index);
	match_vassert(
		"c:\\halo\\SOURCE\\cache\\xbox_sound_cache.c",
		0x141,
		cache_sound->software_reference_count == 0 &&
			cache_sound->hardware_reference_count == 0,
		csprintf(
			temporary,
			"tried to delete sound %s(%s) from the cache while it was playing.",
			tag_get_name(cache_sound->sound->runtime_tag_index),
			cache_sound->sound));
	match_assert(
		"c:\\halo\\SOURCE\\cache\\xbox_sound_cache.c",
		0x144,
		cache_sound->sound->cache_block_index==block_index);
	cache_sound->sound->cache_block_index = NONE;
	cache_sound->sound->cache_base_address = 0;
	datum_delete(xbox_sound_cache_globals.cache_sounds, block_index);

	return;
}

static const char *cache_block_get_sound_permutation_name(
	long block_index)
{
	struct xbox_cache_sound_datum *cache_sound;

	cache_sound = datum_get(xbox_sound_cache_globals.cache_sounds, block_index);
	sprintf(
		xbox_sound_cache_globals.debug_block_name,
		"%s (%s)",
		tag_get_name(cache_sound->sound->runtime_tag_index),
		cache_sound->sound);

	return xbox_sound_cache_globals.debug_block_name;
}

static void sound_cache_start_loading_sound(
	struct sound_permutation *sound)
{
	long cache_block_index;

	cache_block_index = lruv_block_new(
		xbox_sound_cache_globals.cache,
		sound->samples.size);
	if (cache_block_index != NONE)
	{
		byte *cache_address;
		long new_cache_sound_index;
		struct xbox_cache_sound_datum *cache_sound;

		cache_address = xbox_sound_cache_globals.base_address +
#ifdef HALO_64BIT
			lruv_block_get_address(
#else
			(unsigned long)lruv_block_get_address(
#endif
				xbox_sound_cache_globals.cache,
				cache_block_index);
		new_cache_sound_index = datum_new_at_index(
			xbox_sound_cache_globals.cache_sounds,
			cache_block_index);
		cache_sound = datum_get(
			xbox_sound_cache_globals.cache_sounds,
			cache_block_index);
		match_assert(
			"c:\\halo\\SOURCE\\cache\\xbox_sound_cache.c",
			0x170,
			new_cache_sound_index==cache_block_index);
		sound->cache_block_index = cache_block_index;
#ifdef HALO_64BIT
		sound->cache_base_address = xbox_address(cache_address);
#else
		sound->cache_base_address = (unsigned long)cache_address;
#endif
		cache_sound->sound = sound;
		cache_file_read(
			sound->cache_tag_index,
			sound->samples.file_offset,
			sound->samples.size,
			cache_address,
			&cache_sound->loaded,
			FALSE);
	}
	else if (
		system_milliseconds() -
			xbox_sound_cache_globals.last_allocation_failure_time > 10000)
	{
		/* (port: chatter, shown as config.toml's game.console_log says) */
		if (terminal_shows(_terminal_message_chatter))
		{
			terminal_printf(
				global_real_argb_purple,
				"!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
		}
		error(
			_error_silent,
			"SOUND CACHE BLOWN!!!! double-click \"GETSTABBED.BAT\" on your PC now!!!");
		/* (port: chatter, shown as config.toml's game.console_log says) */
		if (terminal_shows(_terminal_message_chatter))
		{
			terminal_printf(
				global_real_argb_purple,
				"!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
		}
		lruv_debug_to_file(
			"d:\\stabbed.txt",
			sound->name,
			sound->samples.size,
			xbox_sound_cache_globals.cache,
			scenario_debug_to_file,
			cache_block_get_sound_permutation_name);
		xbox_sound_cache_globals.last_allocation_failure_time =
			system_milliseconds();
	}

	return;
}

boolean _sound_cache_sound_request(
	struct sound_permutation *sound,
	boolean block,
	boolean load,
	boolean reference)
{
	boolean result = FALSE;

	match_assert(
		"c:\\halo\\SOURCE\\cache\\xbox_sound_cache.c",
		0xC2,
		load || !block);
	match_assert(
		"c:\\halo\\SOURCE\\cache\\xbox_sound_cache.c",
		0xC4,
		load || !reference);
	match_assert(
		"c:\\halo\\SOURCE\\cache\\xbox_sound_cache.c",
		0xC6,
		sound->cache_tag_index!=0);

	if (sound->cache_block_index == NONE && load)
	{
		sound_cache_start_loading_sound(sound);
	}

	if (sound->cache_block_index != NONE)
	{
		lruv_block_touch(
			xbox_sound_cache_globals.cache,
			sound->cache_block_index);
		do
		{
			struct xbox_cache_sound_datum *cache_sound = datum_get(
				xbox_sound_cache_globals.cache_sounds,
				sound->cache_block_index);
			if (cache_sound->loaded)
			{
				if (!cache_sound->initialized)
				{
					cache_sound->initialized = TRUE;
					cache_sound->software_reference_count = 0;
					cache_sound->hardware_reference_count = 0;
				}

				if (reference)
				{
					if (debug_sound_reference_counts)
					{
						error(
							_error_silent,
							"--- request %d %s",
							cache_sound->software_reference_count,
							cache_sound->sound);
					}
					match_assert(
						"c:\\halo\\SOURCE\\cache\\xbox_sound_cache.c",
						0xEC,
						cache_sound->software_reference_count<UNSIGNED_CHAR_MAX);
					cache_sound->software_reference_count++;
				}

				result = TRUE;
			}
			else
			{
				SwitchToThread();
			}
		}
		while (!result && block);
	}

	return result;
}

static void render_inverse_transform_screen_point(
	real_point2d const *screen_position,
	real_point3d *world_position,
	real_vector3d *world_vector)
{
	real screen_x;
	real screen_y;
	real_vector3d top_edge;
	real_vector3d left_edge;
	real_point3d temp_point;

	screen_x = screen_position->x / 640.0f;
	screen_y = 1.0f - screen_position->y / 480.0f;
	point_from_line3d(
		&render.frustum.world_vertices[4],
		global_zero_vector3d,
		1.0f,
		world_position);
	vector_from_points3d(
		&render.frustum.world_vertices[0],
		&render.frustum.world_vertices[1],
		&top_edge);
	vector_from_points3d(
		&render.frustum.world_vertices[0],
		&render.frustum.world_vertices[2],
		&left_edge);
	point_from_line3d(
		&render.frustum.world_vertices[0],
		&top_edge,
		screen_x,
		&temp_point);
	point_from_line3d(
		&temp_point,
		&left_edge,
		screen_y,
		&temp_point);
	vector_from_points3d(
		world_position,
		&temp_point,
		world_vector);

	return;
}

void sound_cache_debug_render(
	void)
{
	if (debug_sound_cache)
	{
		short rows = 1024 / 640;
		byte page_usage[1024];
		long x;
		real_argb_color const *colors[4];

		colors[0] = global_real_argb_red;
		colors[1] = global_real_argb_green;
		colors[2] = global_real_argb_blue;
		colors[3] = global_real_argb_yellow;
		lruv_cache_get_page_usage(xbox_sound_cache_globals.cache, page_usage);

		for (x = 0; x < 640; x++)
		{
			short page_index = rows * x;
			long bit;

			for (bit = 0; bit < 4; bit++)
			{
				if (TEST_FLAG(page_usage[page_index], bit))
				{
					real_point3d extent[2];
					real_point2d line_pt[2];
					long i;
					real_vector3d vec;

					line_pt[0].x = (real)(x % 640);
					line_pt[0].y = (real)((bit + (x / 640) * 4) * 10);
					line_pt[1].x = (real)(x % 640);
					line_pt[1].y = (real)((bit + (x / 640) * 4 + 1) * 10);
					for (i = 0; i < 2; i++)
					{
						render_inverse_transform_screen_point(
							&line_pt[i],
							&extent[i],
							&vec);
						point_from_line3d(
							&extent[i],
							&vec,
							(render.camera.z_near + 0.001f) / dot_product3d(&vec, &render.camera.forward),
							&extent[i]);
					}

					render_debug_line(
						TRUE,
						&extent[0],
						&extent[1],
						colors[bit]);
				}
			}
		}
	}

	return;
}
