/*
XBOX_MEMORY.C

#ifdef HALO_64BIT
The Xbox address space, Xbox contiguous memory (XPhysicalAlloc) and page
protection for the modern 64-bit build.
#else
Xbox contiguous memory (XPhysicalAlloc) and page protection for the Linux
build.
#endif

#ifdef HALO_64BIT
At start-up the layer reserves 4 GB at XBOX_ADDRESS_SPACE_BASE
(halo_xbox_address.h), so Xbox address X is host address base + X. On the
Xbox, physical memory at address P is visible at virtual address
0x80000000 + P; the game asks for its game state and tag cache at fixed
#else
On the Xbox, physical memory at address P is visible at virtual address
0x80000000 + P. The game asks for its game state and tag cache at fixed
#endif
addresses that way (physical_memory_map.c), and Direct3D resources carry
#ifdef HALO_64BIT
physical addresses in their Data fields. That contiguous window is committed
read-write up front and handed out in 4 KB Xbox pages: placed requests at
exactly the address asked for, the rest top-down as the Xbox kernel does.

Host pages may be larger than the Xbox's (16 KB on Apple silicon), so page
protection is applied to the host pages a range covers completely, and a
freshly allocated block is cleared rather than remapped.
#else
physical addresses in their Data fields. A 32-bit Linux process on a 64-bit
kernel owns the whole 4 GB address space, so the layer reserves the same
virtual window at start-up and allocates page-granular blocks inside it:
placed requests at exactly the address asked for, the rest top-down as the
Xbox kernel does.
#endif
*/

#include "platform.h"

#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifdef HALO_64BIT
#define PAGE_SIZE_BYTES 0x1000U
#else
#define PAGE_SIZE_BYTES 0x1000UL
#endif
#define CONTIGUOUS_PAGE_COUNT (PLATFORM_CONTIGUOUS_SIZE / PAGE_SIZE_BYTES)

/* per page: 0 free, otherwise the protection of the block (PAGE_*); the
first page of a block also records the block length */
static DWORD page_protection[CONTIGUOUS_PAGE_COUNT];
static unsigned long block_page_count[CONTIGUOUS_PAGE_COUNT];
static BOOL arena_reserved = FALSE;
static pthread_mutex_t arena_lock = PTHREAD_MUTEX_INITIALIZER;
#ifdef HALO_64BIT

unsigned int platform_host_page_size = PAGE_SIZE_BYTES;
#endif

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
#ifdef HALO_64BIT
static void xbox_address_space_reserve(void)
#else
static void contiguous_arena_reserve(void)
#endif
{
#ifdef HALO_64BIT
	void *wanted = (void *)XBOX_ADDRESS_SPACE_BASE;
	void *result;
	long host_page_size = sysconf(_SC_PAGESIZE);
#else
	void *wanted = (void *)PLATFORM_CONTIGUOUS_BASE;
	void *result = mmap(wanted, PLATFORM_CONTIGUOUS_SIZE, PROT_NONE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
#endif

#ifdef HALO_64BIT
	if (host_page_size > 0)
		platform_host_page_size = (unsigned int)host_page_size;
	result = mmap(wanted, XBOX_ADDRESS_SPACE_SIZE, PROT_NONE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (result != wanted)
#else
	if (result == wanted)
	{
		arena_reserved = TRUE;
	}
	else
#endif
	{
		if (result != MAP_FAILED)
#ifdef HALO_64BIT
			munmap(result, XBOX_ADDRESS_SPACE_SIZE);
		platform_log("cannot reserve the Xbox address space at %p (%s)", wanted, strerror(errno));
		abort();
#else
			munmap(result, PLATFORM_CONTIGUOUS_SIZE);
		platform_log("cannot reserve the Xbox contiguous memory window at %p (%s)",
			wanted, strerror(errno));
#endif
	}
#ifdef HALO_64BIT
	if (mmap(xbox_pointer(PLATFORM_CONTIGUOUS_BASE), PLATFORM_CONTIGUOUS_SIZE, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != xbox_pointer(PLATFORM_CONTIGUOUS_BASE))
	{
		platform_log("cannot commit the Xbox contiguous memory window (%s)", strerror(errno));
		abort();
	}
	arena_reserved = TRUE;
}

void xbox_address_out_of_range(void const *pointer)
{
	platform_log("pointer %p is outside the Xbox address space and cannot be stored in 32 bits", pointer);
	abort();
#endif
}

BOOL platform_is_contiguous(const void *address)
{
#ifdef HALO_64BIT
	unsigned long long offset = (unsigned long long)(uintptr_t)address - XBOX_ADDRESS_SPACE_BASE;
#else
	unsigned long value = (unsigned long)address;
#endif

#ifdef HALO_64BIT
	return offset >= PLATFORM_CONTIGUOUS_BASE && offset - PLATFORM_CONTIGUOUS_BASE < PLATFORM_CONTIGUOUS_SIZE;
#else
	return value >= PLATFORM_CONTIGUOUS_BASE && value - PLATFORM_CONTIGUOUS_BASE < PLATFORM_CONTIGUOUS_SIZE;
#endif
}

#ifdef HALO_64BIT
static unsigned int contiguous_page(const void *address)
#else
static BOOL pages_free(unsigned long first, unsigned long count)
#endif
{
#ifdef HALO_64BIT
	return (xbox_address(address) - PLATFORM_CONTIGUOUS_BASE) / PAGE_SIZE_BYTES;
}

/* Change the host protection of the host pages that [address, address +
size) covers completely; returns FALSE (with errno) on failure. */
static BOOL protect_host_pages(void *address, size_t size, int protection)
{
	uintptr_t mask = platform_host_page_size - 1;
	uintptr_t start = ((uintptr_t)address + mask) & ~mask;
	uintptr_t end = ((uintptr_t)address + size) & ~mask;

	if (end <= start)
		return TRUE;
	return mprotect((void *)start, end - start, protection) == 0;
}

static BOOL pages_free(unsigned int first, unsigned int count)
{
	unsigned int page;
#else
	unsigned long page;
#endif

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

#ifdef HALO_64BIT
	address = xbox_pointer(PLATFORM_CONTIGUOUS_BASE + first * PAGE_SIZE_BYTES);
#else
	address = (void *)(PLATFORM_CONTIGUOUS_BASE + first * PAGE_SIZE_BYTES);
#endif
	memory_watch_forget(address, count * PAGE_SIZE_BYTES);
#ifdef HALO_64BIT
	/* the window is committed read-write; a block starts out zeroed */
	memset(address, 0, count * PAGE_SIZE_BYTES);
	if (protection_to_host(protect) != (PROT_READ | PROT_WRITE))
		protect_host_pages(address, count * PAGE_SIZE_BYTES, protection_to_host(protect));
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
#ifdef HALO_64BIT
	first = contiguous_page(address);
#else
	first = ((unsigned long)address - PLATFORM_CONTIGUOUS_BASE) / PAGE_SIZE_BYTES;
#endif
	pthread_mutex_lock(&arena_lock);
	count = block_page_count[first];
	if (count)
	{
		memory_watch_forget(address, count * PAGE_SIZE_BYTES);
#ifdef HALO_64BIT
		protect_host_pages(address, count * PAGE_SIZE_BYTES, PROT_READ | PROT_WRITE);
#else
		mmap(address, count * PAGE_SIZE_BYTES, PROT_NONE,
			MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
#endif
		for (page = first; page < first + count; page++)
			page_protection[page] = 0;
		block_page_count[first] = 0;
#ifdef HALO_64BIT
	}
	pthread_mutex_unlock(&arena_lock);
}

/* The host protection a host page should have: that of its Xbox pages when
they all agree (protect_host_pages only protects fully covered pages),
read-write otherwise. */
static int host_page_protection(uintptr_t host_page)
{
	unsigned int first = contiguous_page((void *)host_page);
	unsigned int count = platform_host_page_size / PAGE_SIZE_BYTES;
	DWORD protect = page_protection[first];
	unsigned int page;

	for (page = first + 1; page < first + count && page < CONTIGUOUS_PAGE_COUNT; page++)
	{
		if (page_protection[page] != protect)
			return PROT_READ | PROT_WRITE;
	}
	return protect ? protection_to_host(protect) : PROT_READ | PROT_WRITE;
}

/* Write into contiguous memory the way the Xbox's DVD and hard disk do:
by DMA, regardless of the CPU's page protection (the game guards its read
buffers PAGE_READONLY while the drive fills them). */
void platform_contiguous_write(void *destination, const void *source, size_t size)
{
	uintptr_t mask = platform_host_page_size - 1;
	uintptr_t start = (uintptr_t)destination & ~mask;
	uintptr_t end = ((uintptr_t)destination + size + mask) & ~mask;
	uintptr_t page;

	if (!size)
		return;
	memory_watch_prepare_write(destination, (unsigned int)size);
	pthread_mutex_lock(&arena_lock);
	mprotect((void *)start, end - start, PROT_READ | PROT_WRITE);
	memcpy(destination, source, size);
	for (page = start; page < end; page += platform_host_page_size)
	{
		int protection = host_page_protection(page);

		if (protection != (PROT_READ | PROT_WRITE))
			mprotect((void *)page, platform_host_page_size, protection);
#endif
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
#ifdef HALO_64BIT
		platform_log("XPhysicalAlloc: cannot allocate %u bytes (physical address 0x%08x)",
			(unsigned int)size, (unsigned int)physical_address);
#else
		platform_log("XPhysicalAlloc: cannot allocate %lu bytes (physical address 0x%08lx)",
			(unsigned long)size, (unsigned long)physical_address);
#endif
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
	}
	return result;
}

VOID WINAPI XPhysicalFree(LPVOID address)
{
	platform_contiguous_free(address);
}

#ifdef HALO_64BIT
/* Protection is per host page, which holds several Xbox pages on Apple
silicon (16 KB): every host page the range touches takes the protection its
Xbox pages share, or read-write where they differ. So a few bytes made
writable in a read-only block (bink_alloc_permanent) are writable. */
BOOL WINAPI VirtualProtect(LPVOID address, SIZE_T size, DWORD new_protect, PDWORD old_protect)
{
	uintptr_t mask = platform_host_page_size - 1;
	uintptr_t first = (uintptr_t)address & ~mask;
	uintptr_t end = ((uintptr_t)address + size + mask) & ~mask;
	uintptr_t host_page;
	unsigned int page, last;

	if (!platform_is_contiguous(address))
	{
		/* outside the Xbox's pages: the host's own pages */
		if (old_protect)
			*old_protect = PAGE_READWRITE;
		memory_watch_forget(address, size);
		if (!protect_host_pages(address, size, protection_to_host(new_protect)))
		{
			platform_set_last_error_from_errno(errno);
			return FALSE;
		}
		return TRUE;
	}
	pthread_mutex_lock(&arena_lock);
	if (old_protect)
		*old_protect = page_protection[contiguous_page(address)];
	last = contiguous_page((char *)address + (size ? size - 1 : 0));
	for (page = contiguous_page(address); page <= last && page < CONTIGUOUS_PAGE_COUNT; page++)
	{
		if (page_protection[page])
			page_protection[page] = new_protect & ~(PAGE_WRITECOMBINE | PAGE_NOCACHE);
	}
	/* (the whole host pages: their protection changes, and with it what the
	memory watch sees) */
	memory_watch_forget((void *)first, (unsigned int)(end - first));
	for (host_page = first; host_page < end; host_page += platform_host_page_size)
	{
		if (mprotect((void *)host_page, platform_host_page_size, host_page_protection(host_page)) != 0)
		{
			pthread_mutex_unlock(&arena_lock);
			platform_set_last_error_from_errno(errno);
			return FALSE;
		}
	}
	pthread_mutex_unlock(&arena_lock);
	return TRUE;
}
#else
BOOL WINAPI VirtualProtect(LPVOID address, SIZE_T size, DWORD new_protect, PDWORD old_protect)
{
	unsigned long start = (unsigned long)address & ~(PAGE_SIZE_BYTES - 1);
	unsigned long end = ((unsigned long)address + size + PAGE_SIZE_BYTES - 1) & ~(PAGE_SIZE_BYTES - 1);

	if (old_protect)
		*old_protect = platform_is_contiguous(address) ?
			page_protection[(start - PLATFORM_CONTIGUOUS_BASE) / PAGE_SIZE_BYTES] : PAGE_READWRITE;
	memory_watch_forget((void *)start, end - start);
	if (mprotect((void *)start, end - start, protection_to_host(new_protect)) != 0)
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
#endif

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
#ifdef HALO_64BIT
		protect = page_protection[contiguous_page(address)];
#else
		protect = page_protection[((unsigned long)address - PLATFORM_CONTIGUOUS_BASE) / PAGE_SIZE_BYTES];
#endif
		pthread_mutex_unlock(&arena_lock);
		if (!protect)
			protect = PAGE_NOACCESS;
	}
	return protect;
}
