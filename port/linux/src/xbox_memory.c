/*
XBOX_MEMORY.C

Xbox contiguous memory (XPhysicalAlloc) and page protection for the Linux
build.

On the Xbox, physical memory at address P is visible at virtual address
0x80000000 + P. The game asks for its game state and tag cache at fixed
addresses that way (physical_memory_map.c), and Direct3D resources carry
physical addresses in their Data fields. A 32-bit Linux process on a 64-bit
kernel owns the whole 4 GB address space, so the layer reserves the same
virtual window at start-up and allocates page-granular blocks inside it:
placed requests at exactly the address asked for, the rest top-down as the
Xbox kernel does.
*/

#include "platform.h"

#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PAGE_SIZE_BYTES 0x1000UL
#define CONTIGUOUS_PAGE_COUNT (PLATFORM_CONTIGUOUS_SIZE / PAGE_SIZE_BYTES)

/* per page: 0 free, otherwise the protection of the block (PAGE_*); the
first page of a block also records the block length */
static DWORD page_protection[CONTIGUOUS_PAGE_COUNT];
static unsigned long block_page_count[CONTIGUOUS_PAGE_COUNT];
static BOOL arena_reserved = FALSE;
static pthread_mutex_t arena_lock = PTHREAD_MUTEX_INITIALIZER;

static int protection_to_host(DWORD protect)
{
	switch (protect & 0xff)
	{
	case PAGE_NOACCESS: return PROT_NONE;
	case PAGE_READONLY: return PROT_READ;
	case PAGE_EXECUTE: return PROT_EXEC;
	case PAGE_EXECUTE_READ: return PROT_READ | PROT_EXEC;
	case PAGE_EXECUTE_READWRITE: return PROT_READ | PROT_WRITE | PROT_EXEC;
	default: return PROT_READ | PROT_WRITE;
	}
}

/* Reserve the window before anything else can map into it. */
__attribute__((constructor(101)))
static void contiguous_arena_reserve(void)
{
#ifdef HALO_WEB
	/* the window is the top of the WebAssembly memory, which the C heap
	stays below (port/web/src/web_main.c) */
	if ((unsigned long long)__builtin_wasm_memory_size(0) * 65536ULL >=
		(unsigned long long)PLATFORM_CONTIGUOUS_BASE + PLATFORM_CONTIGUOUS_SIZE)
		arena_reserved = TRUE;
	else
		platform_log("the WebAssembly memory does not reach the Xbox contiguous memory window");
	return;
#endif
	void *wanted = (void *)PLATFORM_CONTIGUOUS_BASE;
	void *result = mmap(wanted, PLATFORM_CONTIGUOUS_SIZE, PROT_NONE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);

	if (result == wanted)
	{
		arena_reserved = TRUE;
	}
	else
	{
		if (result != MAP_FAILED)
			munmap(result, PLATFORM_CONTIGUOUS_SIZE);
		platform_log("cannot reserve the Xbox contiguous memory window at %p (%s)",
			wanted, strerror(errno));
	}
}

BOOL platform_is_contiguous(const void *address)
{
	unsigned long value = (unsigned long)address;

	return value >= PLATFORM_CONTIGUOUS_BASE && value - PLATFORM_CONTIGUOUS_BASE < PLATFORM_CONTIGUOUS_SIZE;
}

static BOOL pages_free(unsigned long first, unsigned long count)
{
	unsigned long page;

	if (first + count > CONTIGUOUS_PAGE_COUNT)
		return FALSE;
	for (page = first; page < first + count; page++)
	{
		if (page_protection[page])
			return FALSE;
	}
	return TRUE;
}

void *platform_contiguous_alloc(unsigned long size, unsigned long alignment,
	unsigned long physical_address, DWORD protect)
{
	unsigned long count = (size + PAGE_SIZE_BYTES - 1) / PAGE_SIZE_BYTES;
	unsigned long alignment_pages = alignment > PAGE_SIZE_BYTES ? alignment / PAGE_SIZE_BYTES : 1;
	unsigned long first = CONTIGUOUS_PAGE_COUNT;
	unsigned long page;
	void *address;

	if (!count)
		count = 1;
	protect &= ~(PAGE_WRITECOMBINE | PAGE_NOCACHE);
	if (!protect)
		protect = PAGE_READWRITE;

	pthread_mutex_lock(&arena_lock);
	if (!arena_reserved)
	{
		pthread_mutex_unlock(&arena_lock);
		return NULL;
	}
	if (physical_address != PLATFORM_ANY_PHYSICAL_ADDRESS)
	{
		unsigned long wanted = (physical_address & ~PLATFORM_CONTIGUOUS_BASE) / PAGE_SIZE_BYTES;

		if (pages_free(wanted, count))
			first = wanted;
	}
	else if (count <= CONTIGUOUS_PAGE_COUNT)
	{
		/* top-down first fit, like the Xbox contiguous allocator */
		unsigned long candidate = CONTIGUOUS_PAGE_COUNT - count;

		for (;;)
		{
			candidate -= candidate % alignment_pages;
			if (pages_free(candidate, count))
			{
				first = candidate;
				break;
			}
			if (candidate == 0)
				break;
			candidate--;
		}
	}
	if (first == CONTIGUOUS_PAGE_COUNT)
	{
		pthread_mutex_unlock(&arena_lock);
		return NULL;
	}

	address = (void *)(PLATFORM_CONTIGUOUS_BASE + first * PAGE_SIZE_BYTES);
	memory_watch_forget(address, count * PAGE_SIZE_BYTES);
#ifdef HALO_WEB
	/* fresh zeroed pages (WebAssembly memory cannot be remapped) */
	memset(address, 0, count * PAGE_SIZE_BYTES);
#else
	/* map fresh zeroed pages over the reservation */
	if (mmap(address, count * PAGE_SIZE_BYTES, protection_to_host(protect),
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != address)
	{
		pthread_mutex_unlock(&arena_lock);
		return NULL;
	}
#endif
	for (page = first; page < first + count; page++)
		page_protection[page] = protect;
	block_page_count[first] = count;
	pthread_mutex_unlock(&arena_lock);
	return address;
}

void platform_contiguous_free(void *address)
{
	unsigned long first, count, page;

	if (!platform_is_contiguous(address))
		return;
	first = ((unsigned long)address - PLATFORM_CONTIGUOUS_BASE) / PAGE_SIZE_BYTES;
	pthread_mutex_lock(&arena_lock);
	count = block_page_count[first];
	if (count)
	{
		memory_watch_forget(address, count * PAGE_SIZE_BYTES);
#ifndef HALO_WEB
		mmap(address, count * PAGE_SIZE_BYTES, PROT_NONE,
			MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
#endif
		for (page = first; page < first + count; page++)
			page_protection[page] = 0;
		block_page_count[first] = 0;
	}
	pthread_mutex_unlock(&arena_lock);
}

/* ---------- XAPI */

LPVOID WINAPI XPhysicalAlloc(SIZE_T size, ULONG_PTR physical_address, ULONG_PTR alignment, DWORD protect)
{
	/* A highest-acceptable address inside the window places the block
	there, which is how the game gets its fixed game state and tag cache
	addresses; anything else may go anywhere. */
	void *result = platform_contiguous_alloc(size, alignment,
		physical_address < PLATFORM_CONTIGUOUS_SIZE ? physical_address : PLATFORM_ANY_PHYSICAL_ADDRESS,
		protect);

	if (!result)
	{
		platform_log("XPhysicalAlloc: cannot allocate %lu bytes (physical address 0x%08lx)",
			(unsigned long)size, (unsigned long)physical_address);
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
	}
	return result;
}

VOID WINAPI XPhysicalFree(LPVOID address)
{
	platform_contiguous_free(address);
}

BOOL WINAPI VirtualProtect(LPVOID address, SIZE_T size, DWORD new_protect, PDWORD old_protect)
{
	unsigned long start = (unsigned long)address & ~(PAGE_SIZE_BYTES - 1);
	unsigned long end = ((unsigned long)address + size + PAGE_SIZE_BYTES - 1) & ~(PAGE_SIZE_BYTES - 1);

	if (old_protect)
		*old_protect = platform_is_contiguous(address) ?
			page_protection[(start - PLATFORM_CONTIGUOUS_BASE) / PAGE_SIZE_BYTES] : PAGE_READWRITE;
	memory_watch_forget((void *)start, end - start);
#ifdef HALO_WEB
	/* WebAssembly memory has no page protection: the pages stay writable */
	if (0)
#else
	if (mprotect((void *)start, end - start, protection_to_host(new_protect)) != 0)
#endif
	{
		platform_set_last_error_from_errno(errno);
		return FALSE;
	}
	if (platform_is_contiguous((void *)start))
	{
		unsigned long page;

		pthread_mutex_lock(&arena_lock);
		for (page = (start - PLATFORM_CONTIGUOUS_BASE) / PAGE_SIZE_BYTES;
			page < (end - PLATFORM_CONTIGUOUS_BASE) / PAGE_SIZE_BYTES && page < CONTIGUOUS_PAGE_COUNT;
			page++)
		{
			if (page_protection[page])
				page_protection[page] = new_protect & ~(PAGE_WRITECOMBINE | PAGE_NOCACHE);
		}
		pthread_mutex_unlock(&arena_lock);
	}
	return TRUE;
}

VOID WINAPI XPhysicalProtect(LPVOID address, SIZE_T size, DWORD new_protect)
{
	VirtualProtect(address, size, new_protect, NULL);
}

DWORD WINAPI XQueryMemoryProtect(LPVOID address)
{
	DWORD protect = PAGE_READWRITE;

	if (platform_is_contiguous(address))
	{
		pthread_mutex_lock(&arena_lock);
		protect = page_protection[((unsigned long)address - PLATFORM_CONTIGUOUS_BASE) / PAGE_SIZE_BYTES];
		pthread_mutex_unlock(&arena_lock);
		if (!protect)
			protect = PAGE_NOACCESS;
	}
	return protect;
}
