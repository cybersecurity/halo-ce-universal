/*
HOST_MEMORY.C

Guest address space for the Android port.

Everything the guest touches must lie below 4 GB, but an Android process
shares that range with ART, whose heap spaces use compressed 32-bit
references and so also live there. The host therefore claims only what the
guest needs, when it needs it:

- the Xbox contiguous window at 0x80000000 and the image's own range, at
  start-up (both at fixed addresses the guest was built for);
- pools of address space for the guest's other mappings (malloc arenas,
  thread stacks), reserved in free gaps below 4 GB as they fill up.

This file also implements guest memory write tracking (the interface of
port/linux/src/memory_watch.c): the renderer write-protects the pages behind
the textures it caches, and the SIGSEGV handler here records the first
write to each. Other faults are reported (with guest-relative addresses) and
passed on to the previous handler. Under ARM translation (the x86 emulator)
page faults cannot be caught, and the tracking compares page contents
instead (host_watch_hash.c).
*/

#include "host.h"
#include "host_watch_hash.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#define PAGE 0x1000ULL
#define LOW_LIMIT 0x100000000ULL
#define LOW_START 0x01000000ULL
#define POOL_SIZE (256ULL * 1024 * 1024)
#define POOL_PAGES (POOL_SIZE / PAGE)
#define MAXIMUM_POOLS 12

struct pool
{
	uint64_t base;
	uint32_t free_pages;
	uint8_t used[POOL_PAGES]; /* 1 for each page handed out */
};

static struct pool *pools[MAXIMUM_POOLS];
static int pool_count;
static pthread_mutex_t memory_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t window_base, window_end;
static uint64_t image_base, image_end;

static uint64_t round_up(uint64_t value)
{
	return (value + PAGE - 1) & ~(PAGE - 1);
}

static int in_range(uint64_t address, uint64_t size, uint64_t base, uint64_t end)
{
	return address >= base && address + size <= end && address + size >= address;
}

/* ---------- reserving address space below 4 GB */

/* the lowest free gap of at least size bytes at or above minimum, from
/proc/self/maps; 0 if none */
static uint64_t find_gap(uint64_t size, uint64_t minimum)
{
	FILE *maps = fopen("/proc/self/maps", "r");
	char line[512];
	uint64_t previous_end = minimum;
	uint64_t result = 0;

	if (!maps)
		return 0;
	while (fgets(line, sizeof(line), maps))
	{
		unsigned long long start, end;

		if (sscanf(line, "%llx-%llx", &start, &end) != 2)
			continue;
		if (end <= previous_end)
			continue;
		if (start > previous_end && start - previous_end >= size)
			break;
		previous_end = end > previous_end ? end : previous_end;
		if (previous_end >= LOW_LIMIT)
			break;
	}
	fclose(maps);
	previous_end = round_up(previous_end);
	if (previous_end + size <= LOW_LIMIT)
		result = previous_end;
	return result;
}

/* ART reserves its spaces (notably the free-list large object space) at
addresses chosen at zygote start; on some devices (for example the Retroid
Pocket Flip2, kernel 4.19) that reservation covers the fixed Xbox memory
window. That space commits pages lazily from its bottom, and the Java side
of this app hardly allocates large objects, so the slice over the window is
normally idle address space: unmap exactly the intersection (never more)
and let the caller retry. Only that space, and only a slice with no page in
use (present or swapped out, from /proc/self/pagemap): ART's other spaces
(its heap, bitmaps and card tables) are live, and unmapping them would
corrupt it where failing to start is at least clear. */
#define ART_LARGE_OBJECT_SPACE "[anon:dalvik-free list large object space]"

/* whether no page from..to (page aligned) is present or swapped out; 0 if
it cannot tell */
static int range_unused(uint64_t from, uint64_t to)
{
	FILE *pagemap = fopen("/proc/self/pagemap", "rb");
	uint64_t entries[512];
	uint64_t page = from / PAGE;
	int unused = 1;

	if (!pagemap)
		return 0;
	if (fseeko(pagemap, (off_t)(page * sizeof(uint64_t)), SEEK_SET) != 0)
		unused = 0;
	while (unused && page < to / PAGE)
	{
		size_t wanted = (size_t)(to / PAGE - page);
		size_t count;
		size_t index;

		if (wanted > sizeof(entries) / sizeof(entries[0]))
			wanted = sizeof(entries) / sizeof(entries[0]);
		count = fread(entries, sizeof(entries[0]), wanted, pagemap);
		if (count != wanted)
		{
			unused = 0;
			break;
		}
		/* (bit 63 present, bit 62 swapped) */
		for (index = 0; index < count; index++)
		{
			if (entries[index] & (3ULL << 62))
			{
				unused = 0;
				break;
			}
		}
		page += count;
	}
	fclose(pagemap);
	return unused;
}

static int reclaim_art_overlap(uint64_t address, uint64_t size)
{
	FILE *maps = fopen("/proc/self/maps", "r");
	char line[512];
	int reclaimed = 0;

	if (!maps)
		return 0;
	while (fgets(line, sizeof(line), maps))
	{
		unsigned long long lo, hi;
		uint64_t from, to;
		char *name;
		int name_offset = 0;

		if (sscanf(line, "%llx-%llx %*s %*s %*s %*s %n", &lo, &hi, &name_offset) < 2 || !name_offset)
			continue;
		if (hi <= address || lo >= address + size)
			continue;
		name = line + name_offset;
		name[strcspn(name, "\n")] = '\0';
		from = lo > address ? lo : address;
		to = hi < address + size ? hi : address + size;
		if (strcmp(name, ART_LARGE_OBJECT_SPACE))
		{
			host_logf(HOST_LOG_ERROR, "a fixed guest range is overlapped by %08llx-%08llx (%s), which is not ART's large object space; left alone",
				lo, hi, name[0] ? name : "unnamed");
			continue;
		}
		if (!range_unused(from, to))
		{
			host_logf(HOST_LOG_ERROR, "ART's large object space over %08llx-%08llx is in use (or its pages cannot be read); left alone",
				(unsigned long long)from, (unsigned long long)to);
			continue;
		}
		if (munmap((void *)from, to - from) == 0)
		{
			host_logf(HOST_LOG_INFO,
				"reclaimed idle ART range %08llx-%08llx (%s)",
				(unsigned long long)from, (unsigned long long)to, name);
			reclaimed = 1;
		}
	}
	fclose(maps);
	return reclaimed;
}

/* reclaim_art: the fixed ranges the guest was built for may take ART's
idle large object space (reclaim_art_overlap); the pools, placed in free
gaps, never do: a mapping in the way there is one ART just made, maybe live */
static int reserve(uint64_t address, uint64_t size, int reclaim_art)
{
	void *result = mmap((void *)address, size, PROT_NONE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);

	if (result == (void *)address)
		return 0;
	if (result != MAP_FAILED)
	{
		munmap(result, size);
		return -1;
	}
	if (reclaim_art && errno == EEXIST)
	{
		int error = errno;

		/* (the caller reports the first failure if nothing was reclaimed) */
		if (!reclaim_art_overlap(address, size))
		{
			errno = error;
			return -1;
		}
		result = mmap((void *)address, size, PROT_NONE,
			MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE,
			-1, 0);
		if (result == (void *)address)
			return 0;
		if (result != MAP_FAILED)
			munmap(result, size);
	}
	return -1;
}

static struct pool *pool_new(void)
{
	/* above the image first: ART allocates its own low-4 GB memory from
	the bottom up */
	uint64_t minimum = image_end ? image_end : LOW_START;
	struct pool *pool;
	int attempt;

	if (pool_count == MAXIMUM_POOLS)
		return NULL;
	for (attempt = 0; attempt < 64; attempt++)
	{
		uint64_t address = find_gap(POOL_SIZE, minimum);

		if (!address && minimum != LOW_START)
		{
			minimum = LOW_START;
			address = find_gap(POOL_SIZE, minimum);
		}
		if (!address)
			return NULL;
		if (reserve(address, POOL_SIZE, 0) == 0)
		{
			pool = calloc(1, sizeof(*pool));
			pool->base = address;
			pool->free_pages = POOL_PAGES;
			pools[pool_count++] = pool;
			host_logf(HOST_LOG_INFO, "guest memory pool %d at %08llx", pool_count - 1, (unsigned long long)address);
			return pool;
		}
		/* raced with another mapping; look further up */
		minimum = address + PAGE;
	}
	return NULL;
}

int host_memory_initialize(uint32_t base, uint32_t size)
{
	window_base = HALO_GUEST_WINDOW_BASE;
	window_end = window_base + HALO_GUEST_WINDOW_SIZE;
	if (reserve(window_base, HALO_GUEST_WINDOW_SIZE, 1) != 0)
	{
		host_logf(HOST_LOG_ERROR, "cannot reserve the Xbox memory window at %08llx (%s)",
			(unsigned long long)window_base, strerror(errno));
		return -1;
	}
	image_base = base;
	image_end = base + round_up(size);
	if (reserve(image_base, image_end - image_base, 1) != 0)
	{
		host_logf(HOST_LOG_ERROR, "cannot reserve the guest image range at %08llx (%s)",
			(unsigned long long)image_base, strerror(errno));
		return -1;
	}
	return 0;
}

/* ---------- page pools */

static void *pool_take(struct pool *pool, uint64_t pages)
{
	uint64_t run = 0, page;

	if (pool->free_pages < pages)
		return NULL;
	for (page = 0; page < POOL_PAGES; page++)
	{
		if (pool->used[page])
		{
			run = 0;
			continue;
		}
		if (++run == pages)
		{
			uint64_t first = page + 1 - pages;

			memset(&pool->used[first], 1, pages);
			pool->free_pages -= pages;
			return (void *)(pool->base + first * PAGE);
		}
	}
	return NULL;
}

void *host_low_map(size_t size, int protection)
{
	uint64_t pages = round_up(size) / PAGE;
	void *address = NULL;
	int index;

	if (!pages || pages > POOL_PAGES)
		return NULL;
	pthread_mutex_lock(&memory_lock);
	for (index = 0; index < pool_count && !address; index++)
		address = pool_take(pools[index], pages);
	if (!address)
	{
		struct pool *pool = pool_new();

		if (pool)
			address = pool_take(pool, pages);
	}
	pthread_mutex_unlock(&memory_lock);
	if (!address)
		return NULL;
	if (mmap(address, pages * PAGE, protection, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != address)
	{
		host_low_unmap(address, pages * PAGE);
		return NULL;
	}
	return address;
}

static struct pool *pool_of(uint64_t address, uint64_t size)
{
	int index;

	for (index = 0; index < pool_count; index++)
	{
		if (in_range(address, size, pools[index]->base, pools[index]->base + POOL_SIZE))
			return pools[index];
	}
	return NULL;
}

void host_low_unmap(void *address, size_t size)
{
	uint64_t start = (uint64_t)address & ~(PAGE - 1);
	uint64_t length = round_up((uint64_t)address + size) - start;
	struct pool *pool;

	pthread_mutex_lock(&memory_lock);
	pool = pool_of(start, length);
	if (pool)
	{
		uint64_t first = (start - pool->base) / PAGE, count = length / PAGE, page;

		/* give the memory back but keep the address space */
		mmap((void *)start, length, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
		for (page = first; page < first + count; page++)
		{
			if (pool->used[page])
			{
				pool->used[page] = 0;
				pool->free_pages++;
			}
		}
	}
	pthread_mutex_unlock(&memory_lock);
}

int host_low_owns(uintptr_t address, size_t size)
{
	int result;

	if (in_range(address, size, window_base, window_end) || in_range(address, size, image_base, image_end))
		return 1;
	pthread_mutex_lock(&memory_lock);
	result = pool_of(address, size) != NULL;
	pthread_mutex_unlock(&memory_lock);
	return result;
}

/* ---------- the guest's memory system calls */

long host_guest_mmap(uint64_t address, uint64_t size, int protection, int flags, int fd, int64_t offset)
{
	uint64_t length = round_up(size);
	void *result;

	if (!length)
		return -EINVAL;
	if (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE))
	{
		int fixed_flags = (flags & ~MAP_FIXED_NOREPLACE) | MAP_FIXED;

		if (address + length > LOW_LIMIT)
			return -ENOMEM;
		/* inside a range the host reserved for the guest, a "no replace"
		request replaces the reservation */
		if (!host_low_owns(address, length))
		{
			if (!(flags & MAP_FIXED_NOREPLACE))
				return -EINVAL;
			fixed_flags = flags;
		}
		result = mmap((void *)address, length, protection, fixed_flags, fd, offset);
		if (result == MAP_FAILED)
			return -errno;
		return (long)(uintptr_t)result;
	}
	result = host_low_map(length, PROT_NONE);
	if (!result)
		return -ENOMEM;
	if (mmap(result, length, protection, flags | MAP_FIXED, fd, offset) != result)
	{
		int error = errno;

		host_low_unmap(result, length);
		return -error;
	}
	return (long)(uintptr_t)result;
}

long host_guest_munmap(uint64_t address, uint64_t size)
{
	uint64_t length = round_up(size);

	if (address + length > LOW_LIMIT)
		return -EINVAL;
	if (in_range(address, length, window_base, window_end))
	{
		mmap((void *)address, length, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
		return 0;
	}
	if (in_range(address, length, image_base, image_end))
		return -EINVAL;
	if (host_low_owns(address, length))
	{
		host_low_unmap((void *)address, length);
		return 0;
	}
	return munmap((void *)address, length) ? -errno : 0;
}

long host_guest_mprotect(uint64_t address, uint64_t size, int protection)
{
	if (address + size > LOW_LIMIT)
		return -EINVAL;
	return mprotect((void *)address, size, protection) ? -errno : 0;
}

/* ---------- write tracking (port/linux/src/memory_watch.c) */

#define WATCH_PAGE_COUNT (HALO_GUEST_WINDOW_SIZE / PAGE)

static uint8_t page_protected[WATCH_PAGE_COUNT];
static uint32_t page_generation[WATCH_PAGE_COUNT];
static volatile uint32_t current_generation = 1;
static int watch_active;
static int watch_hashing;
static uint8_t page_hash_watched[WATCH_PAGE_COUNT];
static uint64_t page_hash[WATCH_PAGE_COUNT];
static uint32_t page_hashed_frame[WATCH_PAGE_COUNT];
static struct watch_hash page_hashes =
{
	.base = (const uint8_t *)(uintptr_t)HALO_GUEST_WINDOW_BASE,
	.page_count = WATCH_PAGE_COUNT,
	.watched = page_hash_watched,
	.hash = page_hash,
	.hashed_frame = page_hashed_frame,
	.generation = page_generation,
	.current_generation = &current_generation,
	.frame = 1,
};

static int in_window(uint64_t address)
{
	return address >= HALO_GUEST_WINDOW_BASE && address - HALO_GUEST_WINDOW_BASE < HALO_GUEST_WINDOW_SIZE;
}

static uint64_t watch_page(uint64_t address)
{
	return (address - HALO_GUEST_WINDOW_BASE) / PAGE;
}

static void mark_written(uint64_t page)
{
	page_generation[page] = __sync_add_and_fetch(&current_generation, 1);
	page_protected[page] = 0;
	mprotect((void *)(HALO_GUEST_WINDOW_BASE + page * PAGE), PAGE, PROT_READ | PROT_WRITE);
}

static struct sigaction previous_segv, previous_bus, previous_ill;

static void report_crash(int signal_number, siginfo_t *information, void *context)
{
	ucontext_t *ucontext = context;
	const struct sigcontext *registers = (const struct sigcontext *)&ucontext->uc_mcontext;
	uint64_t pc = registers->pc, lr = registers->regs[30];
	int index;

	host_logf(HOST_LOG_ERROR, "signal %d at address %p: pc %016llx lr %016llx sp %016llx",
		signal_number, information->si_addr, (unsigned long long)pc, (unsigned long long)lr,
		(unsigned long long)registers->sp);
	if (pc >= host_image.base && pc < host_image.end)
		host_logf(HOST_LOG_ERROR, "  in the guest image: addr2line -e halo_guest.elf 0x%llx 0x%llx",
			(unsigned long long)pc, (unsigned long long)lr);
	for (index = 0; index < 31; index += 4)
	{
		host_logf(HOST_LOG_ERROR, "  x%-2d %016llx %016llx %016llx %016llx", index,
			(unsigned long long)registers->regs[index],
			(unsigned long long)(index + 1 < 31 ? registers->regs[index + 1] : 0),
			(unsigned long long)(index + 2 < 31 ? registers->regs[index + 2] : 0),
			(unsigned long long)(index + 3 < 31 ? registers->regs[index + 3] : 0));
	}
	/* the guest's frame records: fp and lr, 8 bytes each */
	{
		uint64_t fp = registers->regs[29];

		for (index = 0; index < 24 && fp && fp < LOW_LIMIT && (fp & 7) == 0; index++)
		{
			const uint64_t *frame = (const uint64_t *)fp;

			if (!host_low_owns(fp, 16))
				break;
			host_logf(HOST_LOG_ERROR, "  frame %2d: return %016llx", index, (unsigned long long)frame[1]);
			fp = frame[0];
		}
	}
}

static void chain(struct sigaction *previous, int signal_number, siginfo_t *information, void *context)
{
	sigaction(signal_number, previous, NULL);
	if (previous->sa_flags & SA_SIGINFO)
	{
		if (previous->sa_sigaction)
			previous->sa_sigaction(signal_number, information, context);
	}
	else if (previous->sa_handler != SIG_DFL && previous->sa_handler != SIG_IGN)
	{
		previous->sa_handler(signal_number);
	}
	/* returning re-executes the faulting instruction under the previous
	(or default) handler */
}

static void segv_handler(int signal_number, siginfo_t *information, void *context)
{
	uint64_t address = (uint64_t)information->si_addr;

	if (watch_active && in_window(address))
	{
		uint64_t page = watch_page(address);

		if (page_protected[page])
		{
			mark_written(page);
			return;
		}
	}
	report_crash(signal_number, information, context);
	chain(&previous_segv, signal_number, information, context);
}

static void bus_handler(int signal_number, siginfo_t *information, void *context)
{
	report_crash(signal_number, information, context);
	chain(&previous_bus, signal_number, information, context);
}

static void ill_handler(int signal_number, siginfo_t *information, void *context)
{
	report_crash(signal_number, information, context);
	chain(&previous_ill, signal_number, information, context);
}

void host_install_signal_handlers(void)
{
	struct sigaction action;

	memset(&action, 0, sizeof(action));
	action.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
	sigemptyset(&action.sa_mask);
	action.sa_sigaction = segv_handler;
	sigaction(SIGSEGV, &action, &previous_segv);
	action.sa_sigaction = bus_handler;
	sigaction(SIGBUS, &action, &previous_bus);
	action.sa_sigaction = ill_handler;
	sigaction(SIGILL, &action, &previous_ill);
}

void host_memory_watch_initialize(void)
{
	watch_active = 1;
}

void host_memory_watch_use_hashes(void)
{
	watch_hashing = 1;
}

/**
 * @brief Starts watching the pages of a range (memory_watch_protect):
 * write-protected, or with their hash kept under host_memory_watch_use_hashes.
 * @param address guest address of the range
 * @param size bytes; ignored outside the guest window
 */
void host_memory_watch_protect(uint32_t address, uint32_t size)
{
	uint64_t first, last, page;

	if (!watch_active || !size || !in_window(address))
		return;
	first = watch_page(address);
	last = watch_page((uint64_t)address + size - 1);
	if (last >= WATCH_PAGE_COUNT)
		last = WATCH_PAGE_COUNT - 1;
	if (watch_hashing)
	{
		watch_hash_protect(&page_hashes, (uint32_t)first, (uint32_t)last);
		return;
	}
	for (page = first; page <= last; page++)
	{
		if (!page_protected[page])
		{
			page_protected[page] = 1;
			mprotect((void *)(HALO_GUEST_WINDOW_BASE + page * PAGE), PAGE, PROT_READ);
		}
	}
}

uint32_t host_memory_watch_serial(void)
{
	return current_generation;
}

/**
 * @brief Finds the newest write to the pages of a range
 * (memory_watch_generation). Under hashing this hashes the pages again,
 * once a frame.
 * @param address guest address of the range
 * @param size bytes
 * @return the newest generation of any page in the range
 */
uint32_t host_memory_watch_generation(uint32_t address, uint32_t size)
{
	uint64_t first, last, page;
	uint32_t newest = 0;

	if (!size || !in_window(address))
		return 0;
	first = watch_page(address);
	last = watch_page((uint64_t)address + size - 1);
	if (last >= WATCH_PAGE_COUNT)
		last = WATCH_PAGE_COUNT - 1;
	if (watch_hashing)
		return watch_hash_generation(&page_hashes, (uint32_t)first, (uint32_t)last);
	for (page = first; page <= last; page++)
	{
		if (page_generation[page] > newest)
			newest = page_generation[page];
	}
	return newest;
}

/**
 * @brief Unprotects the pages the host itself is about to write into
 * (memory_watch_prepare_write). Nothing under hashing: the write shows in
 * the next hash and there is no protection to fault on.
 * @param address guest address of the range
 * @param size bytes
 */
void host_memory_watch_prepare_write(uint32_t address, uint32_t size)
{
	uint64_t start = address, first, last, page;

	if (!watch_active || watch_hashing || !size)
		return;
	if (start + size <= HALO_GUEST_WINDOW_BASE || start >= (uint64_t)HALO_GUEST_WINDOW_BASE + HALO_GUEST_WINDOW_SIZE)
		return;
	if (start < HALO_GUEST_WINDOW_BASE)
		start = HALO_GUEST_WINDOW_BASE;
	first = watch_page(start);
	last = watch_page((uint64_t)address + size - 1);
	if (last >= WATCH_PAGE_COUNT)
		last = WATCH_PAGE_COUNT - 1;
	for (page = first; page <= last; page++)
	{
		if (page_protected[page])
			mark_written(page);
	}
}

/**
 * @brief Treats the pages of a range as written and unwatched
 * (memory_watch_forget), because they were remapped or reprotected.
 * @param address guest address of the range
 * @param size bytes
 */
void host_memory_watch_forget(uint32_t address, uint32_t size)
{
	uint64_t first, last, page;

	if (!size || !in_window(address))
		return;
	first = watch_page(address);
	last = watch_page((uint64_t)address + size - 1);
	if (last >= WATCH_PAGE_COUNT)
		last = WATCH_PAGE_COUNT - 1;
	if (watch_hashing)
	{
		watch_hash_forget(&page_hashes, (uint32_t)first, (uint32_t)last);
		return;
	}
	for (page = first; page <= last; page++)
	{
		page_protected[page] = 0;
		page_generation[page] = __sync_add_and_fetch(&current_generation, 1);
	}
}

/**
 * @brief A new frame starts (memory_watch_begin_frame): pages may be
 * hashed again. Nothing under page protection.
 */
void host_memory_watch_begin_frame(void)
{
	if (watch_hashing)
		watch_hash_begin_frame(&page_hashes);
}
