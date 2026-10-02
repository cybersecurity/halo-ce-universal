/*
WEB_MEMORY_WATCH.C

Write tracking for guest memory that the renderer caches
(port/linux/src/memory_watch.c's interface).

WebAssembly memory has no page protection, so a write cannot fault. A
watched page instead remembers a hash of its contents: asking for its
generation hashes it again, and a different hash counts as a write, as the
fault would have. Writes the file layer announces (memory_watch_prepare_write)
count at once. The renderer only asks about the pages its draws read, so
the pages hashed each frame are about the ones the GPU reads anyway.
*/

#include "platform.h"

#include <stdint.h>
#include <string.h>

#define WATCH_PAGE_SIZE 0x1000UL
#define WATCH_PAGE_COUNT (PLATFORM_CONTIGUOUS_SIZE / WATCH_PAGE_SIZE)

static unsigned char page_protected[WATCH_PAGE_COUNT];
static unsigned long page_generation[WATCH_PAGE_COUNT];
static uint64_t page_hash[WATCH_PAGE_COUNT];
static volatile unsigned long current_generation = 1;

static unsigned long page_index(unsigned long address)
{
	return (address - PLATFORM_CONTIGUOUS_BASE) / WATCH_PAGE_SIZE;
}

static uint64_t hash_page(unsigned long page)
{
	const uint64_t *words = (const uint64_t *)(PLATFORM_CONTIGUOUS_BASE + page * WATCH_PAGE_SIZE);
	uint64_t a = 0x9e3779b97f4a7c15ULL, b = 0xc2b2ae3d27d4eb4fULL;
	uint64_t c = 0x165667b19e3779f9ULL, d = 0x27d4eb2f165667c5ULL;
	unsigned long index;

	/* four independent lanes, so the loop vectorises */
	for (index = 0; index < WATCH_PAGE_SIZE / sizeof(uint64_t); index += 4)
	{
		a = (a ^ words[index + 0]) * 0x100000001b3ULL;
		b = (b ^ words[index + 1]) * 0x100000001b3ULL;
		c = (c ^ words[index + 2]) * 0x100000001b3ULL;
		d = (d ^ words[index + 3]) * 0x100000001b3ULL;
	}
	return a ^ (b << 1 | b >> 63) ^ (c << 7 | c >> 57) ^ (d << 13 | d >> 51);
}

static void mark_written(unsigned long page)
{
	page_generation[page] = __sync_add_and_fetch(&current_generation, 1);
	page_protected[page] = 0;
}

static BOOL page_range(unsigned long address, unsigned long size, unsigned long *first, unsigned long *last)
{
	unsigned long start = address, end = address + size;

	if (!size || end <= PLATFORM_CONTIGUOUS_BASE || start >= PLATFORM_CONTIGUOUS_BASE + PLATFORM_CONTIGUOUS_SIZE)
		return FALSE;
	if (start < PLATFORM_CONTIGUOUS_BASE)
		start = PLATFORM_CONTIGUOUS_BASE;
	if (end > PLATFORM_CONTIGUOUS_BASE + PLATFORM_CONTIGUOUS_SIZE)
		end = PLATFORM_CONTIGUOUS_BASE + PLATFORM_CONTIGUOUS_SIZE;
	*first = page_index(start);
	*last = page_index(end - 1);
	return TRUE;
}

void memory_watch_initialize(void)
{
}

void memory_watch_protect(unsigned long address, unsigned long size)
{
	unsigned long first, last, page;

	if (!page_range(address, size, &first, &last))
		return;
	for (page = first; page <= last; page++)
	{
		if (!page_protected[page])
		{
			page_hash[page] = hash_page(page);
			page_protected[page] = 1;
		}
	}
}

unsigned long memory_watch_generation(unsigned long address, unsigned long size)
{
	unsigned long first, last, page, newest = 0;

	if (!page_range(address, size, &first, &last))
		return 0;
	for (page = first; page <= last; page++)
	{
		if (page_protected[page] && hash_page(page) != page_hash[page])
			mark_written(page);
		if (page_generation[page] > newest)
			newest = page_generation[page];
	}
	return newest;
}

unsigned long memory_watch_serial(void)
{
	return current_generation;
}

void memory_watch_prepare_write(void *address, unsigned long size)
{
	unsigned long first, last, page;

	if (!page_range((unsigned long)address, size, &first, &last))
		return;
	for (page = first; page <= last; page++)
	{
		if (page_protected[page])
			mark_written(page);
	}
}

void memory_watch_forget(void *address, unsigned long size)
{
	unsigned long first, last, page;

	if (!page_range((unsigned long)address, size, &first, &last))
		return;
	for (page = first; page <= last; page++)
	{
		page_protected[page] = 0;
		page_generation[page] = __sync_add_and_fetch(&current_generation, 1);
	}
}
