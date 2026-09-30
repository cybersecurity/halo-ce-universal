/*
HOST_WATCH_HASH.C

Write tracking by page contents (host_watch_hash.h).
*/

#include "host_watch_hash.h"

/*
FNV-style mixing in four independent lanes, so that the multiplies of
one lane do not wait for another's. A change of one word changes its
lane's state for good (xor and an odd multiply are both invertible), so
a single-word write always shows. Changes of several words could cancel
each other, which is unlikely for game data. (host_watch_hash.h)
*/
uint64_t watch_hash_page(const uint8_t *page)
{
	const uint64_t *words = (const uint64_t *)page;
	uint64_t lane0 = 0xcbf29ce484222325ULL, lane1 = 0x9e3779b97f4a7c15ULL;
	uint64_t lane2 = 0xc2b2ae3d27d4eb4fULL, lane3 = 0x165667b19e3779f9ULL;
	uint32_t index;

	for (index = 0; index < WATCH_HASH_PAGE_SIZE / 8; index += 4)
	{
		lane0 = (lane0 ^ words[index + 0]) * 0x100000001b3ULL;
		lane1 = (lane1 ^ words[index + 1]) * 0x100000001b3ULL;
		lane2 = (lane2 ^ words[index + 2]) * 0x100000001b3ULL;
		lane3 = (lane3 ^ words[index + 3]) * 0x100000001b3ULL;
	}
	return lane0 ^ ((lane1 << 16) | (lane1 >> 48)) ^ ((lane2 << 32) | (lane2 >> 32)) ^
		((lane3 << 48) | (lane3 >> 16));
}

/**
 * @brief Clamps a page range to the watched memory.
 * @param watch the tracking state
 * @param first first page of the range
 * @param last in: last page; out: last page inside the memory
 * @return 0 when nothing is left of the range
 */
static int clamp(const struct watch_hash *watch, uint32_t first, uint32_t *last)
{
	if (first >= watch->page_count || first > *last)
		return 0;
	if (*last >= watch->page_count)
		*last = watch->page_count - 1;
	return 1;
}

/**
 * @brief Finds the memory of a page.
 * @param watch the tracking state
 * @param page the page number
 * @return the page's first byte
 */
static const uint8_t *page_memory(const struct watch_hash *watch, uint32_t page)
{
	return watch->base + (uint64_t)page * WATCH_HASH_PAGE_SIZE;
}

void watch_hash_protect(struct watch_hash *watch, uint32_t first, uint32_t last)
{
	uint32_t page;

	if (!clamp(watch, first, &last))
		return;
	for (page = first; page <= last; page++)
	{
		if (watch->watched[page])
			continue;
		watch->watched[page] = 1;
		watch->hash[page] = watch_hash_page(page_memory(watch, page));
		watch->hashed_frame[page] = watch->frame;
	}
}

uint32_t watch_hash_generation(struct watch_hash *watch, uint32_t first, uint32_t last)
{
	uint32_t page, newest = 0;

	if (!clamp(watch, first, &last))
		return 0;
	for (page = first; page <= last; page++)
	{
		if (watch->watched[page] && watch->hashed_frame[page] != watch->frame)
		{
			uint64_t hash = watch_hash_page(page_memory(watch, page));

			watch->hashed_frame[page] = watch->frame;
			if (hash != watch->hash[page])
			{
				watch->hash[page] = hash;
				watch->generation[page] = __sync_add_and_fetch(watch->current_generation, 1);
			}
		}
		if (watch->generation[page] > newest)
			newest = watch->generation[page];
	}
	return newest;
}

void watch_hash_forget(struct watch_hash *watch, uint32_t first, uint32_t last)
{
	uint32_t page;

	if (!clamp(watch, first, &last))
		return;
	for (page = first; page <= last; page++)
	{
		watch->watched[page] = 0;
		watch->generation[page] = __sync_add_and_fetch(watch->current_generation, 1);
	}
}

/*
The serial changes each frame, so a cache that skips generation queries
while the serial stays the same (xbox_textures.c's recent_textures) asks
again once a frame. (host_watch_hash.h)
*/
void watch_hash_begin_frame(struct watch_hash *watch)
{
	watch->frame++;
	if (!watch->frame)
		watch->frame = 1;
	__sync_add_and_fetch(watch->current_generation, 1);
}
