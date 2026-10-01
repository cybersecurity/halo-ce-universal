/*
LRU_CACHE.C

symbols in this file:
0010C7F0 0010:
	_lru_default_new_block_proc (0000)
0010C800 0010:
	_lru_default_purge_block_proc (0000)
0010C810 0020:
	_get_lru_cache_block_offset (0000)
0010C830 0080:
	_verify_lru_cache_block (0000)
0010C8B0 0080:
	_verify_lru_cache (0000)
0010C930 0140:
	_lru_new (0000)
0010CA70 0050:
	_lru_dispose (0000)
0010CAC0 0060:
	_lru_flush (0000)
0010CB20 0020:
	_lru_free_blocks (0000)
0010CB40 00d0:
	_lru_allocate (0000)
0010CC10 0030:
	_lru_lock (0000)
0010CC40 0030:
	_lru_unlock (0000)
0010CC70 0030:
	_lru_touch (0000)
0027DF18 0031:
	??_C@_0DB@CMCOIKGJ@lru?5cache?5?$CFs?5?$EA?$CFp?5block?5?$EA?$CFp?5appea@ (0000)
0027DF4C 0022:
	??_C@_0CC@FJMKCKIJ@c?3?2halo?2SOURCE?2memory?2lru_cache?4@ (0000)
0027DF70 0027:
	??_C@_0CH@KBEEMIKM@lru?5cache?5?$CFs?5?$EA?$CFp?5appears?5to?5be?5c@ (0000)
0027DF98 0017:
	??_C@_0BH@JCHCKBCG@total_size?$DO?$DNblock_size?$AA@ (0000)
0027DFB0 000e:
	??_C@_0O@OEDJFKAE@block_size?$DO?$DN0?$AA@ (0000)
*/

/* ---------- headers */

#include "cseries/cseries.h"
#include "cseries/errors.h"
#include "lru_cache.h"

/* ---------- constants */

enum
{
	_lru_block_header_size = 0x10,
	_lru_cache_signature = 0x6C727563,
	_lru_block_signature = 0x55626C6A,
};

/* ---------- macros */

/* ---------- structures */

struct lru_cache_block
{
	void *user_data;
	unsigned long flags;
	unsigned long age;
	long unused;
};

struct lru_cache
{
	char name[32];
	long maximum_block_count;
	long block_size;
	long total_size;
	lru_block_new_proc block_new_proc;
	lru_block_delete_proc block_delete_proc;
	struct lru_cache_block *blocks;
	boolean owns_blocks;
	byte __pad39[3];
	unsigned long next_age;
	long block_count;
	unsigned long signature;
};
#ifndef HALO_64BIT

typedef char lru_cache_size_assert[sizeof(struct lru_cache) == 0x48 ? 1 : -1];
typedef char lru_cache_block_size_assert[sizeof(struct lru_cache_block) == 0x10 ? 1 : -1];
#endif

/* ---------- prototypes */

static void lru_default_new_block_proc(
	void *reference,
	void *block);
static void lru_default_purge_block_proc(
	void *reference);
static void verify_lru_cache_block(
	struct lru_cache *cache,
	struct lru_cache_block *block);
static long get_lru_cache_block_offset(
	struct lru_cache *cache,
	struct lru_cache_block *block);
static void verify_lru_cache(
	struct lru_cache *cache);

/* ---------- globals */

/* ---------- public code */

static void lru_default_new_block_proc(
	void *reference,
	void *block)
{
	*(void **)reference = block;

	return;
}

static void lru_default_purge_block_proc(
	void *reference)
{
	*(void **)reference = NULL;

	return;
}

struct lru_cache *lru_new(
	char const *name,
	long total_size,
	long block_size,
	lru_block_new_proc block_new_proc,
	lru_block_delete_proc block_delete_proc,
	void *blocks)
{
	struct lru_cache *cache;
	long maximum_block_count;
	boolean owns_blocks;

	match_assert(
		"c:\\halo\\SOURCE\\memory\\lru_cache.c",
		0x5E,
		block_size>=0);
	match_assert(
		"c:\\halo\\SOURCE\\memory\\lru_cache.c",
		0x5F,
		total_size>=block_size);

	if (!block_new_proc || !block_delete_proc)
	{
		block_new_proc = lru_default_new_block_proc;
		block_delete_proc = lru_default_purge_block_proc;
	}

	block_size += sizeof(struct lru_cache_block);
	if (block_size & 3)
	{
		block_size = (block_size | 3) + 1;
	}
	maximum_block_count = total_size / block_size;

	cache = debug_malloc(
		sizeof(*cache),
		FALSE,
		"c:\\halo\\SOURCE\\memory\\lru_cache.c",
		0x6E);
	if (cache)
	{
		owns_blocks = FALSE;
		if (!blocks)
		{
			blocks = debug_malloc(
				maximum_block_count * block_size,
				FALSE,
				"c:\\halo\\SOURCE\\memory\\lru_cache.c",
				0x75);
			owns_blocks = TRUE;
		}

		if (blocks)
		{
			csmemset(cache, 0, sizeof(*cache));
			cache->maximum_block_count = maximum_block_count;
			cache->blocks = blocks;
			cache->block_new_proc = block_new_proc;
			cache->next_age = 0;
			cache->block_count = 0;
			cache->block_size = block_size;
			cache->total_size = maximum_block_count * block_size;
			cache->signature = _lru_cache_signature;
			cache->block_delete_proc = block_delete_proc;
			cache->owns_blocks = owns_blocks;
			csstrncpy(cache->name, name, 31);
			cache->name[31] = 0;
			verify_lru_cache(cache);
		}
		else
		{
			debug_free(
				cache,
				"c:\\halo\\SOURCE\\memory\\lru_cache.c",
				0x8E);

			return NULL;
		}
	}

	return cache;
}

void lru_dispose(
	struct lru_cache *cache)
{
	verify_lru_cache(cache);
	if (cache->owns_blocks)
	{
		debug_free(
			cache->blocks,
			"c:\\halo\\SOURCE\\memory\\lru_cache.c",
			0x9C);
	}

	debug_free(
		cache,
		"c:\\halo\\SOURCE\\memory\\lru_cache.c",
		0x9D);

	return;
}

void lru_flush(
	struct lru_cache *cache)
{
	long block_index;
	struct lru_cache_block *block;

	verify_lru_cache(cache);
	block = cache->blocks;
	for (
		block_index = 0;
		block_index < cache->block_count;
		block_index++, block = (struct lru_cache_block *)((byte *)block + cache->block_size))
	{
		verify_lru_cache_block(cache, block);
		cache->block_delete_proc(block->user_data);
	}
	cache->block_count = 0;

	return;
}

long lru_free_blocks(
	struct lru_cache *cache)
{
	verify_lru_cache(cache);

	return cache->maximum_block_count - cache->block_count;
}

void *lru_allocate(
	struct lru_cache *cache,
	void *user_data)
{
	long block_index;
	unsigned long oldest_age;
	struct lru_cache_block *block;
	struct lru_cache_block *candidate = NULL;
	void *result = NULL;

	verify_lru_cache(cache);
	if (cache->block_count == cache->maximum_block_count)
	{
		block = cache->blocks;
		for (
			block_index = 0;
			block_index < cache->block_count;
			block_index++, block = (struct lru_cache_block *)((byte *)block + cache->block_size))
		{
			verify_lru_cache_block(cache, block);
			if (!(block->flags & FLAG(0)) &&
				(!candidate || oldest_age > block->age))
			{
				candidate = block;
				oldest_age = block->age;
			}
		}

		if (candidate)
		{
			cache->block_delete_proc(candidate->user_data);
		}
	}
	else
	{
		candidate = (struct lru_cache_block *)
			((byte *)cache->blocks + cache->block_count * cache->block_size);
		cache->block_count++;
	}

	if (candidate)
	{
		candidate->user_data = user_data;
		candidate->flags = _lru_block_signature;
		candidate->age = cache->next_age;
		cache->next_age++;
		candidate->unused = 0;
		result = candidate + 1;
		cache->block_new_proc(candidate->user_data, result);
	}

	return result;
}

void lru_lock(
	struct lru_cache *cache,
	void *block)
{
	struct lru_cache_block *header = (struct lru_cache_block *)block - 1;

	verify_lru_cache(cache);
	verify_lru_cache_block(cache, header);
	header->flags |= FLAG(0);

	return;
}

void lru_unlock(
	struct lru_cache *cache,
	void *block)
{
	struct lru_cache_block *header = (struct lru_cache_block *)block - 1;

	verify_lru_cache(cache);
	verify_lru_cache_block(cache, header);
	header->flags &= ~FLAG(0);

	return;
}

void lru_touch(
	struct lru_cache *cache,
	void *block)
{
	struct lru_cache_block *header = (struct lru_cache_block *)block - 1;

	verify_lru_cache(cache);
	verify_lru_cache_block(cache, header);
	header->age = cache->next_age;
	cache->next_age++;

	return;
}

/* ---------- private code */

static long get_lru_cache_block_offset(
	struct lru_cache *cache,
	struct lru_cache_block *block)
{
	verify_lru_cache_block(cache, block);

	return (byte *)block-(byte *)cache->blocks;
}

static void verify_lru_cache_block(
	struct lru_cache *cache,
	struct lru_cache_block *block)
{
	long offset;
	boolean valid = FALSE;

	if ((block->flags & ~FLAG(0)) == _lru_block_signature && !block->unused)
	{
		get_lru_cache_block_offset(cache, block);
		offset = (byte *)block - (byte *)cache->blocks;
		if (offset >= 0 &&
			cache->block_size + offset <= cache->total_size &&
			block->age < cache->next_age)
		{
			valid = TRUE;
		}
	}

	match_vassert(
		"c:\\halo\\SOURCE\\memory\\lru_cache.c",
		0x156,
		valid,
		csprintf(
			temporary,
			"lru cache %s @%p block @%p appears to be corrupt",
			cache,
			cache,
			block));

	return;
}

static void verify_lru_cache(
	struct lru_cache *cache)
{
	boolean valid =
		cache->signature == _lru_cache_signature &&
		cache->blocks &&
		cache->block_new_proc &&
		cache->block_delete_proc &&
		cache->total_size == cache->maximum_block_count * cache->block_size &&
		(unsigned long)cache->block_size >= _lru_block_header_size &&
		cache->maximum_block_count >= 0 &&
		cache->block_count >= 0 &&
		cache->block_count <= cache->maximum_block_count;

	match_vassert(
		"c:\\halo\\SOURCE\\memory\\lru_cache.c",
		0x16B,
		valid,
		csprintf(
			temporary,
			"lru cache %s @%p appears to be corrupt",
			cache,
			cache));

	return;
}
