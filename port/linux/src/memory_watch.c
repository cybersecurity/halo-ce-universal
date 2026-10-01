/*
MEMORY_WATCH.C

Write tracking for guest memory that the renderer caches.

Textures live in the Xbox contiguous window, where the game (or its
streaming threads) can rewrite them at any time. Instead of hashing their
contents every frame, the pages behind a cached texture are made read-only;
the first write faults, the handler records a new generation for the page,
makes it writable again and lets the write proceed. A cache entry is stale
when any of its pages has a generation newer than the entry.
#ifdef HALO_64BIT

Tracking works in host pages (16 KB on Apple silicon), so a write next to a
texture can mark it stale; that costs a re-upload, never a missed change.
#endif

Writes that the kernel performs on the game's behalf (read() into a
buffer) would fail with EFAULT instead of faulting, so the file layer reads
guest memory through a bounce buffer (xbox_files.c). Unprotecting ahead of
such a write (memory_watch_prepare_write) is not enough on its own: the
renderer can protect the pages again before the kernel writes them.
*/

#include "platform.h"

#include <execinfo.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#ifndef HALO_64BIT
#include <ucontext.h>
#endif
#include <sys/mman.h>
#ifdef HALO_64BIT
#include <sys/ucontext.h>
#endif
#include <unistd.h>

#ifdef HALO_64BIT
/* enough entries for 4 KB host pages; larger pages use fewer */
#define WATCH_PAGE_COUNT_MAXIMUM (PLATFORM_CONTIGUOUS_SIZE / 0x1000U)
#else
#define WATCH_PAGE_SIZE 0x1000UL
#define WATCH_PAGE_COUNT (PLATFORM_CONTIGUOUS_SIZE / WATCH_PAGE_SIZE)
#endif

#ifdef HALO_64BIT
static unsigned char page_protected[WATCH_PAGE_COUNT_MAXIMUM];
static unsigned int page_generation[WATCH_PAGE_COUNT_MAXIMUM];
static volatile unsigned int current_generation = 1;
#else
static unsigned char page_protected[WATCH_PAGE_COUNT];
static unsigned long page_generation[WATCH_PAGE_COUNT];
static volatile unsigned long current_generation = 1;
#endif
static struct sigaction previous_segv_action;
#ifdef HALO_64BIT
static struct sigaction previous_bus_action;
#endif
static BOOL watch_active = FALSE;
#ifdef HALO_64BIT
static unsigned int watch_page_size = 0x1000U;
static unsigned int watch_page_count = WATCH_PAGE_COUNT_MAXIMUM;
#endif

#ifdef HALO_64BIT
static BOOL xbox_address_is_contiguous(unsigned int address)
#else
static unsigned long page_index(unsigned long address)
#endif
{
#ifdef HALO_64BIT
	return address >= PLATFORM_CONTIGUOUS_BASE && address - PLATFORM_CONTIGUOUS_BASE < PLATFORM_CONTIGUOUS_SIZE;
#else
	return (address - PLATFORM_CONTIGUOUS_BASE) / WATCH_PAGE_SIZE;
#endif
}

#ifdef HALO_64BIT
static unsigned int page_index(unsigned int address)
{
	return (address - PLATFORM_CONTIGUOUS_BASE) / watch_page_size;
}

static void *page_pointer(unsigned int page)
{
	return xbox_pointer(PLATFORM_CONTIGUOUS_BASE + page * watch_page_size);
}

static void mark_written(unsigned int page)
#else
static void mark_written(unsigned long page)
#endif
{
	page_generation[page] = __sync_add_and_fetch(&current_generation, 1);
	page_protected[page] = 0;
#ifdef HALO_64BIT
	mprotect(page_pointer(page), watch_page_size, PROT_READ | PROT_WRITE);
#else
	mprotect((void *)(PLATFORM_CONTIGUOUS_BASE + page * WATCH_PAGE_SIZE), WATCH_PAGE_SIZE, PROT_READ | PROT_WRITE);
#endif
}

/* errors.c's: debug.txt (a player sends it; the terminal's lines may be
gone) */
void write_to_error_file(char *string, unsigned char date);

/* a line of a crash report (ending in a newline) to debug.txt too, best
effort: the crash may be in the middle of writing it */
static void crash_debug_line(const char *line)
{
	char text[200];
	size_t length = strcspn(line, "\n");

	if (length > sizeof(text) - 3)
		length = sizeof(text) - 3;
	memcpy(text, line, length);
	memcpy(text + length, "\r\n", 3);
	write_to_error_file(text, 1);
}

#ifdef HALO_64BIT
static void report_crash(siginfo_t *information, void *context)
#else
static void segv_handler(int signal_number, siginfo_t *information, void *context)
#endif
{
#ifdef HALO_64BIT
	ucontext_t *ucontext = context;
	char line[160];
	void *frames[48];
	int count, length;
	unsigned long long pc = 0, frame = 0, stack = 0;
#else
	unsigned long address = (unsigned long)information->si_addr;
#endif

#ifdef HALO_64BIT
#if defined(__APPLE__) && defined(__aarch64__)
	pc = ucontext->uc_mcontext->__ss.__pc;
	frame = ucontext->uc_mcontext->__ss.__fp;
	stack = ucontext->uc_mcontext->__ss.__sp;
#elif defined(__APPLE__) && defined(__x86_64__)
	pc = ucontext->uc_mcontext->__ss.__rip;
	frame = ucontext->uc_mcontext->__ss.__rbp;
	stack = ucontext->uc_mcontext->__ss.__rsp;
#elif defined(__linux__) && defined(__aarch64__)
	pc = ucontext->uc_mcontext.pc;
	frame = ucontext->uc_mcontext.regs[29];
	stack = ucontext->uc_mcontext.sp;
#elif defined(__linux__) && defined(__x86_64__)
	pc = ucontext->uc_mcontext.gregs[REG_RIP];
	frame = ucontext->uc_mcontext.gregs[REG_RBP];
	stack = ucontext->uc_mcontext.gregs[REG_RSP];
#endif
	length = snprintf(line, sizeof(line), "halo: fault at %p, pc %llx fp %llx sp %llx\n",
		information->si_addr, pc, frame, stack);
	write(STDERR_FILENO, line, (size_t)length);
	count = backtrace(frames, 48);
	backtrace_symbols_fd(frames, count, STDERR_FILENO);
}

static void chain(struct sigaction *previous, int signal_number, siginfo_t *information, void *context)
{
	sigaction(signal_number, previous, NULL);
	if (previous->sa_flags & SA_SIGINFO)
#else
	if (platform_is_contiguous((void *)address))
#endif
	{
#ifdef HALO_64BIT
		if (previous->sa_sigaction)
			previous->sa_sigaction(signal_number, information, context);
	}
	else if (previous->sa_handler != SIG_DFL && previous->sa_handler != SIG_IGN)
	{
		previous->sa_handler(signal_number);
	}
	/* returning re-executes the faulting instruction under the old handler */
}

/* macOS reports a write to a read-only page as SIGBUS, Linux as SIGSEGV */
static void fault_handler(int signal_number, siginfo_t *information, void *context)
{
	unsigned long long offset = (unsigned long long)(uintptr_t)information->si_addr - XBOX_ADDRESS_SPACE_BASE;

	if (offset < XBOX_ADDRESS_SPACE_SIZE && xbox_address_is_contiguous((unsigned int)offset))
	{
		unsigned int page = page_index((unsigned int)offset);
#else
		unsigned long page = page_index(address);
#endif

		if (page_protected[page])
		{
			mark_written(page);
			return;
		}
	}
#ifdef HALO_64BIT
	/* a genuine crash: report it, then hand it to whatever handled the
	signal before */
	report_crash(information, context);
	chain(signal_number == SIGBUS ? &previous_bus_action : &previous_segv_action,
		signal_number, information, context);
#else
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
		crash_debug_line(line);
		{
			/* the return address a call through a bad pointer left behind */
			const unsigned *stack = (const unsigned *)ucontext->uc_mcontext.gregs[REG_ESP];

			length = snprintf(line, sizeof(line), "halo-linux: stack %08x %08x %08x %08x %08x %08x\n",
				stack[0], stack[1], stack[2], stack[3], stack[4], stack[5]);
			write(STDERR_FILENO, line, (size_t)length);
			crash_debug_line(line);
		}
		count = backtrace(frames, 48);
		backtrace_symbols_fd(frames, count, STDERR_FILENO);
		{
			int frame;

			for (frame = 0; frame < count; frame++)
			{
				snprintf(line, sizeof(line), "halo-linux: called from %p\n", frames[frame]);
				crash_debug_line(line);
			}
		}
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
#endif
}

void memory_watch_initialize(void)
{
	struct sigaction action;

	if (watch_active)
		return;
#ifdef HALO_64BIT
	watch_page_size = platform_host_page_size;
	watch_page_count = PLATFORM_CONTIGUOUS_SIZE / watch_page_size;
#endif
	memset(&action, 0, sizeof(action));
#ifdef HALO_64BIT
	action.sa_sigaction = fault_handler;
#else
	action.sa_sigaction = segv_handler;
#endif
	action.sa_flags = SA_SIGINFO | SA_NODEFER;
	sigemptyset(&action.sa_mask);
#ifdef HALO_64BIT
	if (sigaction(SIGSEGV, &action, &previous_segv_action) == 0 &&
		sigaction(SIGBUS, &action, &previous_bus_action) == 0)
	{
#else
	if (sigaction(SIGSEGV, &action, &previous_segv_action) == 0)
#endif
		watch_active = TRUE;
#ifdef HALO_64BIT
	}
#endif
}

#ifdef HALO_64BIT
/* the watched pages [first, last] a range touches; FALSE if none */
static BOOL page_range(unsigned int address, unsigned int size, unsigned int *first, unsigned int *last)
#else
void memory_watch_protect(unsigned long address, unsigned long size)
#endif
{
#ifdef HALO_64BIT
	unsigned int end;
#else
	unsigned long first, last, page;
#endif

#ifdef HALO_64BIT
	if (!size || !xbox_address_is_contiguous(address))
		return FALSE;
	end = address + size - 1;
	if (end < address || !xbox_address_is_contiguous(end))
		end = PLATFORM_CONTIGUOUS_BASE + PLATFORM_CONTIGUOUS_SIZE - 1;
	*first = page_index(address);
	*last = page_index(end);
	return TRUE;
}

void memory_watch_protect(unsigned int address, unsigned int size)
{
	unsigned int first, last, page;

	if (!watch_active || !page_range(address, size, &first, &last))
#else
	if (!watch_active || !size || !platform_is_contiguous((void *)address))
#endif
		return;
#ifndef HALO_64BIT
	first = page_index(address);
	last = page_index(address + size - 1);
	if (last >= WATCH_PAGE_COUNT)
		last = WATCH_PAGE_COUNT - 1;
#endif
	for (page = first; page <= last; page++)
	{
		if (!page_protected[page])
		{
			page_protected[page] = 1;
#ifdef HALO_64BIT
			mprotect(page_pointer(page), watch_page_size, PROT_READ);
#else
			mprotect((void *)(PLATFORM_CONTIGUOUS_BASE + page * WATCH_PAGE_SIZE), WATCH_PAGE_SIZE, PROT_READ);
#endif
		}
	}
}

unsigned long memory_watch_generation(unsigned long address, unsigned long size)
{
	unsigned long first, last, page, newest = 0;

#ifdef HALO_64BIT
	if (!page_range(address, size, &first, &last))
#else
	if (!size || !platform_is_contiguous((void *)address))
#endif
		return 0;
#ifndef HALO_64BIT
	first = page_index(address);
	last = page_index(address + size - 1);
	if (last >= WATCH_PAGE_COUNT)
		last = WATCH_PAGE_COUNT - 1;
#endif
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

#ifdef HALO_64BIT
/* the part of a host range inside the contiguous window, as Xbox addresses */
static BOOL clip_to_window(void *address, unsigned int size, unsigned int *start, unsigned int *clipped_size)
{
	unsigned long long begin = (unsigned long long)(uintptr_t)address - XBOX_ADDRESS_SPACE_BASE;
	unsigned long long end = begin + size;
	unsigned long long window_begin = PLATFORM_CONTIGUOUS_BASE;
	unsigned long long window_end = window_begin + PLATFORM_CONTIGUOUS_SIZE;

	if ((uintptr_t)address < XBOX_ADDRESS_SPACE_BASE || !size || end <= window_begin || begin >= window_end)
		return FALSE;
	if (begin < window_begin)
		begin = window_begin;
	if (end > window_end)
		end = window_end;
	*start = (unsigned int)begin;
	*clipped_size = (unsigned int)(end - begin);
	return TRUE;
}
#endif

void memory_watch_prepare_write(void *address, unsigned long size)
{
#ifdef HALO_64BIT
	unsigned int start, clipped_size, first, last, page;
#else
	unsigned long start = (unsigned long)address;
	unsigned long first, last, page;
#endif

#ifdef HALO_64BIT
	if (!watch_active || !clip_to_window(address, size, &start, &clipped_size) ||
		!page_range(start, clipped_size, &first, &last))
	{
#else
	if (!watch_active || !size)
#endif
		return;
#ifdef HALO_64BIT
	}
#else
	if (start + size <= PLATFORM_CONTIGUOUS_BASE || start >= PLATFORM_CONTIGUOUS_BASE + PLATFORM_CONTIGUOUS_SIZE)
		return;
	if (start < PLATFORM_CONTIGUOUS_BASE)
		start = PLATFORM_CONTIGUOUS_BASE;
	first = page_index(start);
	last = page_index((unsigned long)address + size - 1);
	if (last >= WATCH_PAGE_COUNT)
		last = WATCH_PAGE_COUNT - 1;
#endif
	for (page = first; page <= last; page++)
	{
		if (page_protected[page])
			mark_written(page);
	}
}

void memory_watch_forget(void *address, unsigned long size)
{
#ifdef HALO_64BIT
	unsigned int start, clipped_size, first, last, page;
#else
	unsigned long start = (unsigned long)address;
	unsigned long first, last, page;
#endif

#ifdef HALO_64BIT
	if (!clip_to_window(address, size, &start, &clipped_size) ||
		!page_range(start, clipped_size, &first, &last))
	{
#else
	if (!size || !platform_is_contiguous(address))
#endif
		return;
#ifdef HALO_64BIT
	}
#else
	first = page_index(start);
	last = page_index(start + size - 1);
	if (last >= WATCH_PAGE_COUNT)
		last = WATCH_PAGE_COUNT - 1;
#endif
	for (page = first; page <= last; page++)
	{
		page_protected[page] = 0;
		page_generation[page] = __sync_add_and_fetch(&current_generation, 1);
	}
}
