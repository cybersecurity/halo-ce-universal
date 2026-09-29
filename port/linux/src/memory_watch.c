/*
MEMORY_WATCH.C

Write tracking for guest memory that the renderer caches.

Textures live in the Xbox contiguous window, where the game (or its
streaming threads) can rewrite them at any time. Instead of hashing their
contents every frame, the pages behind a cached texture are made read-only;
the first write faults, the handler records a new generation for the page,
makes it writable again and lets the write proceed. A cache entry is stale
when any of its pages has a generation newer than the entry.

Writes that the kernel performs on the game's behalf (read() into a
buffer) would fail with EFAULT instead of faulting, so the file layer reads
guest memory through a bounce buffer (xbox_files.c). Unprotecting ahead of
such a write (memory_watch_prepare_write) is not enough on its own: the
renderer can protect the pages again before the kernel writes them.
*/

#if defined(HALO_MACOS) && defined(HALO_MEMORY_WATCH_TEST)
#include "memory_watch_test_platform.h"
#else
#include "platform.h"
#endif

#ifdef HALO_MACOS

#ifdef HALO_MEMORY_WATCH_TEST_ARENA
extern unsigned char halo_watch_test_arena[];
#undef PLATFORM_CONTIGUOUS_BASE
#define PLATFORM_CONTIGUOUS_BASE ((unsigned long)halo_watch_test_arena)
#undef PLATFORM_CONTIGUOUS_SIZE
#define PLATFORM_CONTIGUOUS_SIZE 0x10000UL
#endif

/*
Darwin's AOT host does not currently install the source port's signal-based
write watch.  Do not report a stable generation in its place: both texture
lookup and the vertex/index mirror use generations to skip reading guest
bytes.  Instead, fingerprint the requested 4 KiB pages on each query.  This
is deliberately a correctness fallback; it trades CPU time for not reusing
GPU copies based on an unwatched page.

There is no mprotect here, and the serial changes on every lookup so the
renderer cannot take its recent-texture shortcut without checking the
resource pages.  `prepare_write` and `forget` invalidate fingerprints so a
write/free/reuse is also observed even if the bytes happen to compare equal
at the first query after the event.
*/

#include <stdint.h>
#include <pthread.h>
#include <string.h>
#ifndef HALO_MEMORY_WATCH_TEST
#include "guest_host.h"
#endif

#define WATCH_PAGE_SIZE 0x1000UL
#define WATCH_PAGE_COUNT (PLATFORM_CONTIGUOUS_SIZE / WATCH_PAGE_SIZE)

struct page_fingerprint
{
	uint64_t first;
	uint64_t second;
	unsigned long generation;
	unsigned char valid;
};

static struct page_fingerprint page_fingerprints[WATCH_PAGE_COUNT];
static volatile unsigned long current_generation = 1;
static pthread_mutex_t fingerprint_lock = PTHREAD_MUTEX_INITIALIZER;

static BOOL watched_span(unsigned long address, unsigned long size,
	unsigned long *first, unsigned long *last)
{
	unsigned long offset;

	if (!size || address < PLATFORM_CONTIGUOUS_BASE)
		return FALSE;
	offset = address - PLATFORM_CONTIGUOUS_BASE;
	if (offset >= PLATFORM_CONTIGUOUS_SIZE || size > PLATFORM_CONTIGUOUS_SIZE - offset)
		return FALSE;
	if (!platform_is_contiguous((void *)address) ||
		!platform_is_contiguous((void *)(address + size - 1)))
		return FALSE;
	*first = offset / WATCH_PAGE_SIZE;
	*last = (offset + size - 1) / WATCH_PAGE_SIZE;
	return *last < WATCH_PAGE_COUNT;
}

#ifdef HALO_MEMORY_WATCH_TEST
static void fingerprint_page(unsigned long page, uint64_t *first, uint64_t *second)
{
#ifndef HALO_MEMORY_WATCH_TEST
	/* Preserve both fingerprints and every lookup scan. Only the expensive
	 * byte loop leaves translated i386 code; no write is assumed absent. */
	unsigned long long hashes[2];
	if (!host_memory_fingerprint_page(PLATFORM_CONTIGUOUS_BASE + page * WATCH_PAGE_SIZE, hashes))
		host_abort("invalid guest span in memory page fingerprint");
	*first = hashes[0];
	*second = hashes[1];
#else
	const volatile unsigned char *bytes = (const volatile unsigned char *)(
		PLATFORM_CONTIGUOUS_BASE + page * WATCH_PAGE_SIZE);
	uint64_t a = UINT64_C(14695981039346656037);
	uint64_t b = UINT64_C(0x9e3779b97f4a7c15);
	unsigned long index;

	for (index = 0; index < WATCH_PAGE_SIZE; index++)
	{
		uint64_t value = bytes[index];
		a = (a ^ value) * UINT64_C(1099511628211);
		b ^= value + UINT64_C(0x9e3779b97f4a7c15) + (b << 6) + (b >> 2);
		b *= UINT64_C(0xbf58476d1ce4e5b9);
	}
	*first = a;
	*second = b;
#endif
}
#endif

void memory_watch_initialize(void)
{
	/* Zero-initialized fingerprints are ready; this backend never protects pages. */
}

void memory_watch_protect(unsigned long address, unsigned long size)
{
	/* No page protection is installed. Every generation query fingerprints bytes. */
	(void)address;
	(void)size;
}

unsigned long memory_watch_generation(unsigned long address, unsigned long size)
{
	unsigned long first_page, last_page, newest = 0;
#ifdef HALO_MEMORY_WATCH_TEST
	unsigned long page;
#else
	typedef char fingerprint_wire_size_must_be_24[sizeof(struct page_fingerprint) == 24 ? 1 : -1];
#endif

	if (!watched_span(address, size, &first_page, &last_page))
		return 0;
	pthread_mutex_lock(&fingerprint_lock);
#ifndef HALO_MEMORY_WATCH_TEST
	/* The native range loop implements the same per-page comparison and
	 * generation updates, including invalidated entries, in one ABI call. */
	if (!host_memory_fingerprint_range(PLATFORM_CONTIGUOUS_BASE + first_page * WATCH_PAGE_SIZE,
		last_page - first_page + 1, &page_fingerprints[first_page], &current_generation, &newest))
		host_abort("invalid guest span in memory range fingerprint");
#else
	for (page = first_page; page <= last_page; page++)
	{
		struct page_fingerprint *entry = &page_fingerprints[page];
		uint64_t first, second;

		fingerprint_page(page, &first, &second);
		if (!entry->valid || entry->first != first || entry->second != second)
		{
			entry->first = first;
			entry->second = second;
			entry->valid = 1;
			entry->generation = __sync_add_and_fetch(&current_generation, 1);
		}
		if (entry->generation > newest)
			newest = entry->generation;
	}
#endif
	pthread_mutex_unlock(&fingerprint_lock);
	return newest;
}

unsigned long memory_watch_serial(void)
{
	/* Force callers to validate their own ranges instead of trusting a global
	serial that cannot identify which guest pages have changed. */
	return __sync_add_and_fetch(&current_generation, 1);
}

static void invalidate_span(unsigned long address, unsigned long size)
{
	unsigned long first_page, last_page, page;

	if (!watched_span(address, size, &first_page, &last_page))
		return;
	pthread_mutex_lock(&fingerprint_lock);
	for (page = first_page; page <= last_page; page++)
	{
		page_fingerprints[page].valid = 0;
		page_fingerprints[page].generation = __sync_add_and_fetch(&current_generation, 1);
	}
	pthread_mutex_unlock(&fingerprint_lock);
}

void memory_watch_prepare_write(void *address, unsigned long size)
{
	invalidate_span((unsigned long)address, size);
}

void memory_watch_forget(void *address, unsigned long size)
{
	invalidate_span((unsigned long)address, size);
}

#else

#include <execinfo.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <ucontext.h>
#include <sys/mman.h>
#include <unistd.h>

#define WATCH_PAGE_SIZE 0x1000UL
#define WATCH_PAGE_COUNT (PLATFORM_CONTIGUOUS_SIZE / WATCH_PAGE_SIZE)

static unsigned char page_protected[WATCH_PAGE_COUNT];
static unsigned long page_generation[WATCH_PAGE_COUNT];
static volatile unsigned long current_generation = 1;
static struct sigaction previous_segv_action;
static BOOL watch_active = FALSE;

static unsigned long page_index(unsigned long address)
{
	return (address - PLATFORM_CONTIGUOUS_BASE) / WATCH_PAGE_SIZE;
}

static void mark_written(unsigned long page)
{
	page_generation[page] = __sync_add_and_fetch(&current_generation, 1);
	page_protected[page] = 0;
	mprotect((void *)(PLATFORM_CONTIGUOUS_BASE + page * WATCH_PAGE_SIZE), WATCH_PAGE_SIZE, PROT_READ | PROT_WRITE);
}

static void segv_handler(int signal_number, siginfo_t *information, void *context)
{
	unsigned long address = (unsigned long)information->si_addr;

	if (platform_is_contiguous((void *)address))
	{
		unsigned long page = page_index(address);

		if (page_protected[page])
		{
			mark_written(page);
			return;
		}
	}
	/* a genuine crash: report it, then hand it to whatever handled SIGSEGV
	before */
	{
		ucontext_t *ucontext = context;
		char line[160];
		void *frames[48];
		int count, length;

		length = snprintf(line, sizeof(line), "halo-linux: segmentation fault at %p, eip %08x ebp %08x esp %08x\n",
			information->si_addr, (unsigned)ucontext->uc_mcontext.gregs[REG_EIP],
			(unsigned)ucontext->uc_mcontext.gregs[REG_EBP], (unsigned)ucontext->uc_mcontext.gregs[REG_ESP]);
		write(STDERR_FILENO, line, (size_t)length);
		{
			/* the return address a call through a bad pointer left behind */
			const unsigned *stack = (const unsigned *)ucontext->uc_mcontext.gregs[REG_ESP];

			length = snprintf(line, sizeof(line), "halo-linux: stack %08x %08x %08x %08x %08x %08x\n",
				stack[0], stack[1], stack[2], stack[3], stack[4], stack[5]);
			write(STDERR_FILENO, line, (size_t)length);
		}
		count = backtrace(frames, 48);
		backtrace_symbols_fd(frames, count, STDERR_FILENO);
	}
	sigaction(SIGSEGV, &previous_segv_action, NULL);
	if (previous_segv_action.sa_flags & SA_SIGINFO)
	{
		if (previous_segv_action.sa_sigaction)
			previous_segv_action.sa_sigaction(signal_number, information, context);
	}
	else if (previous_segv_action.sa_handler != SIG_DFL && previous_segv_action.sa_handler != SIG_IGN)
	{
		previous_segv_action.sa_handler(signal_number);
	}
	/* returning re-executes the faulting instruction under the old handler */
}

void memory_watch_initialize(void)
{
	struct sigaction action;

	if (watch_active)
		return;
	memset(&action, 0, sizeof(action));
	action.sa_sigaction = segv_handler;
	action.sa_flags = SA_SIGINFO | SA_NODEFER;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGSEGV, &action, &previous_segv_action) == 0)
		watch_active = TRUE;
}

void memory_watch_protect(unsigned long address, unsigned long size)
{
	unsigned long first, last, page;

	if (!watch_active || !size || !platform_is_contiguous((void *)address))
		return;
	first = page_index(address);
	last = page_index(address + size - 1);
	if (last >= WATCH_PAGE_COUNT)
		last = WATCH_PAGE_COUNT - 1;
	for (page = first; page <= last; page++)
	{
		if (!page_protected[page])
		{
			page_protected[page] = 1;
			mprotect((void *)(PLATFORM_CONTIGUOUS_BASE + page * WATCH_PAGE_SIZE), WATCH_PAGE_SIZE, PROT_READ);
		}
	}
}

unsigned long memory_watch_generation(unsigned long address, unsigned long size)
{
	unsigned long first, last, page, newest = 0;

	if (!size || !platform_is_contiguous((void *)address))
		return 0;
	first = page_index(address);
	last = page_index(address + size - 1);
	if (last >= WATCH_PAGE_COUNT)
		last = WATCH_PAGE_COUNT - 1;
	for (page = first; page <= last; page++)
	{
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
	unsigned long start = (unsigned long)address;
	unsigned long first, last, page;

	if (!watch_active || !size)
		return;
	if (start + size <= PLATFORM_CONTIGUOUS_BASE || start >= PLATFORM_CONTIGUOUS_BASE + PLATFORM_CONTIGUOUS_SIZE)
		return;
	if (start < PLATFORM_CONTIGUOUS_BASE)
		start = PLATFORM_CONTIGUOUS_BASE;
	first = page_index(start);
	last = page_index((unsigned long)address + size - 1);
	if (last >= WATCH_PAGE_COUNT)
		last = WATCH_PAGE_COUNT - 1;
	for (page = first; page <= last; page++)
	{
		if (page_protected[page])
			mark_written(page);
	}
}

void memory_watch_forget(void *address, unsigned long size)
{
	unsigned long start = (unsigned long)address;
	unsigned long first, last, page;

	if (!size || !platform_is_contiguous(address))
		return;
	first = page_index(start);
	last = page_index(start + size - 1);
	if (last >= WATCH_PAGE_COUNT)
		last = WATCH_PAGE_COUNT - 1;
	for (page = first; page <= last; page++)
	{
		page_protected[page] = 0;
		page_generation[page] = __sync_add_and_fetch(&current_generation, 1);
	}
}

#endif /* HALO_MACOS */
