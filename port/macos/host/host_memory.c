/*
HOST_MEMORY.C

Guest address space for the macOS port.

The guest addresses 4 GB: a region of the host's address space starting at
host_guest_base (host.h). The x86-64 host is linked with a 64 KB
__PAGEZERO, so its low 4 GB is ordinary address space and the region is the
low 4 GB itself (host_guest_base 0); the arm64 host reserves a 4 GB-aligned
region anywhere. Either way the host reserves it at start-up, before
anything else can map memory there (a constructor, ahead of main and of
SDL), without access. Everything the guest uses is carved from its range
[GUEST_LOW, GUEST_HIGH):

- the Xbox contiguous window at 0x80000000 and the image's own range, at
  the fixed guest addresses the guest was built for;
- pages for the guest's other mappings (malloc arenas, thread stacks), from
  a page map over the rest.

No guest memory lies below GUEST_LOW (256 MB at least): the OpenGL thunks
tell buffer offsets from pointers by that (tools/android_gl_stubs.py).
Memory the guest gives back is returned to the system but the address space
stays reserved.

This file also implements guest memory write tracking (the interface of
port/linux/src/memory_watch.c): the renderer write-protects the pages behind
the textures it caches, and the fault handler here records the first write
to each. On macOS a write to a read-only page arrives as SIGBUS as well as
SIGSEGV.
*/

#include "host.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <unistd.h>

#define REGION_SIZE 0x100000000ULL
#ifdef __aarch64__
/* the image at HALO_GUEST_IMAGE_BASE (0x88000000), above the window */
#define GUEST_LOW 0x10000000ULL
#define PAGE 0x4000ULL
#else
/* the image at HALO_GUEST_IMAGE_BASE (0x20000000); the host executable
and the libraries the loader maps early lie below */
#define GUEST_LOW 0x20000000ULL
#define PAGE 0x1000ULL
#endif
#define GUEST_HIGH 0xfff00000ULL
#define REGION_PAGES ((GUEST_HIGH - GUEST_LOW) / PAGE)

uint64_t host_guest_base;

/* 1 for each page handed out, or part of the window or the image */
static uint8_t *page_used;
static uint64_t search_hint;
static int reserved;
static pthread_mutex_t memory_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t window_base, window_end;
static uint64_t image_base, image_end;

size_t host_guest_page_size(void)
{
	return PAGE;
}

static uint64_t round_up(uint64_t value)
{
	return (value + PAGE - 1) & ~(PAGE - 1);
}

static int in_range(uint64_t address, uint64_t size, uint64_t base, uint64_t end)
{
	return address >= base && address + size <= end && address + size >= address;
}

/* guest range -> no access, address space kept */
static void map_none(uint64_t address, uint64_t size)
{
	mmap(GUEST(void *, address), size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
}

/* ---------- the reservation */

int host_memory_reserve(void)
{
	if (reserved)
		return 0;
#ifdef __aarch64__
	{
		/* twice the size, to align a region inside it */
		char *area = mmap(NULL, 2 * REGION_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
		uint64_t start, base;

		if (area == MAP_FAILED)
			return -1;
		start = (uint64_t)(uintptr_t)area;
		base = (start + REGION_SIZE - 1) & ~(REGION_SIZE - 1);
		if (base > start)
			munmap(area, base - start);
		if (base + REGION_SIZE < start + 2 * REGION_SIZE)
			munmap((void *)(uintptr_t)(base + REGION_SIZE), start + 2 * REGION_SIZE - (base + REGION_SIZE));
		host_guest_base = base;
	}
#else
	{
		/* not MAP_FIXED: that would replace whatever is already there */
		void *result = mmap((void *)GUEST_LOW, GUEST_HIGH - GUEST_LOW, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);

		if (result != (void *)GUEST_LOW)
		{
			if (result != MAP_FAILED)
				munmap(result, GUEST_HIGH - GUEST_LOW);
			return -1;
		}
		host_guest_base = 0;
	}
#endif
	page_used = calloc(REGION_PAGES, 1);
	if (!page_used)
		return -1;
	reserved = 1;
	return 0;
}

/* as early as possible: before SDL, the dynamic loader's later work or the
allocator can take any of the range */
__attribute__((constructor(101))) static void reserve_early(void)
{
	host_memory_reserve();
}

static void mark(uint64_t address, uint64_t size, uint8_t value)
{
	uint64_t first = (address - GUEST_LOW) / PAGE;
	uint64_t count = round_up(size) / PAGE;

	memset(page_used + first, value, count);
}

int host_memory_initialize(uint32_t base, uint32_t size)
{
	if (host_memory_reserve() != 0)
	{
		host_logf(HOST_LOG_ERROR, "cannot reserve the guest's memory (%s)", strerror(errno));
		return -1;
	}
	window_base = HALO_GUEST_WINDOW_BASE;
	window_end = window_base + HALO_GUEST_WINDOW_SIZE;
	image_base = base;
	image_end = base + round_up(size);
	if (!in_range(window_base, HALO_GUEST_WINDOW_SIZE, GUEST_LOW, GUEST_HIGH) ||
		!in_range(image_base, image_end - image_base, GUEST_LOW, GUEST_HIGH) ||
		(image_base < window_end && image_end > window_base))
	{
		host_logf(HOST_LOG_ERROR, "the guest image %08llx-%08llx does not fit the reserved region",
			image_base, image_end);
		return -1;
	}
	/* the window is usable from the start (host_guest_mmap) */
	if (mmap(GUEST(void *, window_base), HALO_GUEST_WINDOW_SIZE, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) != GUEST(void *, window_base))
	{
		host_logf(HOST_LOG_ERROR, "cannot map the Xbox memory window (%s)", strerror(errno));
		return -1;
	}
	pthread_mutex_lock(&memory_lock);
	mark(window_base, HALO_GUEST_WINDOW_SIZE, 1);
	mark(image_base, image_end - image_base, 1);
	search_hint = image_end;
	pthread_mutex_unlock(&memory_lock);
	host_logf(HOST_LOG_INFO, "guest memory at %016llx, %llu KB pages", (unsigned long long)host_guest_base,
		(unsigned long long)PAGE / 1024);
	return 0;
}

/* ---------- pages */

/* the first run of free pages at or after start (guest addresses); 0 if
none */
static uint64_t find_run(uint64_t pages, uint64_t start)
{
	uint64_t first = (start - GUEST_LOW) / PAGE, page, run = 0;

	for (page = first; page < REGION_PAGES; page++)
	{
		if (page_used[page])
		{
			run = 0;
			continue;
		}
		if (++run == pages)
			return GUEST_LOW + (page + 1 - pages) * PAGE;
	}
	return 0;
}

/* pages for the guest, as a guest address; 0 if none */
static uint64_t take(uint64_t length)
{
	uint64_t address;

	pthread_mutex_lock(&memory_lock);
	address = find_run(length / PAGE, search_hint ? search_hint : GUEST_LOW);
	if (!address)
		address = find_run(length / PAGE, GUEST_LOW);
	if (address)
	{
		mark(address, length, 1);
		search_hint = address + length;
	}
	pthread_mutex_unlock(&memory_lock);
	return address;
}

static int in_pool(uint64_t address, uint64_t size)
{
	return in_range(address, size, GUEST_LOW, GUEST_HIGH) &&
		!in_range(address, size, window_base, window_end) && !in_range(address, size, image_base, image_end);
}

static void give_back(uint64_t address, uint64_t size)
{
	uint64_t start = address & ~(PAGE - 1);
	uint64_t length = round_up(address + size) - start;

	if (!in_pool(start, length))
		return;
	map_none(start, length);
	pthread_mutex_lock(&memory_lock);
	mark(start, length, 0);
	if (start < search_hint)
		search_hint = start;
	pthread_mutex_unlock(&memory_lock);
}

void *host_low_map(size_t size, int protection)
{
	uint64_t length = round_up(size);
	uint64_t address;
	void *host;

	if (!length || !reserved)
		return NULL;
	address = take(length);
	if (!address)
		return NULL;
	host = GUEST(void *, address);
	if (mmap(host, length, protection, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) != host)
	{
		give_back(address, length);
		return NULL;
	}
	return host;
}

void host_low_unmap(void *address, size_t size)
{
	give_back(GUEST_ADDRESS(address), size);
}

int host_low_owns(uint64_t address, uint64_t size)
{
	uint64_t first, last, page;
	int result = 1;

	if (in_range(address, size, window_base, window_end) || in_range(address, size, image_base, image_end))
		return 1;
	if (!size || !in_range(address, size, GUEST_LOW, GUEST_HIGH))
		return 0;
	first = (address - GUEST_LOW) / PAGE;
	last = (address + size - 1 - GUEST_LOW) / PAGE;
	pthread_mutex_lock(&memory_lock);
	for (page = first; page <= last; page++)
	{
		if (!page_used[page])
		{
			result = 0;
			break;
		}
	}
	pthread_mutex_unlock(&memory_lock);
	return result;
}

/* ---------- the Xbox window

The game places blocks at fixed addresses in the window (its tag cache and
game state go where the Xbox had them), aligned to the Xbox's 4 KB pages,
which are smaller than Apple silicon's 16 KB ones. So the window stays
mapped, readable and writable, and the guest's mappings and unmappings in
it (port/linux/src/xbox_memory.c) only give it fresh zeroed memory: whole
host pages are mapped anew, the parts of pages at either end cleared. */

static void window_refresh(uint64_t address, uint64_t size)
{
	uint64_t first = round_up(address);
	uint64_t last = (address + size) & ~(PAGE - 1);

	if (first < last)
	{
		mmap(GUEST(void *, first), last - first, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
		if (address < first)
			memset(GUEST(void *, address), 0, first - address);
		if (address + size > last)
			memset(GUEST(void *, last), 0, address + size - last);
	}
	else
	{
		/* within one host page */
		memset(GUEST(void *, address), 0, size);
	}
}

/* ---------- the guest's memory system calls (guest addresses, Linux flag
values) */

#define LINUX_MAP_SHARED 0x01
#define LINUX_MAP_PRIVATE 0x02
#define LINUX_MAP_FIXED 0x10
#define LINUX_MAP_ANONYMOUS 0x20
#define LINUX_MAP_FIXED_NOREPLACE 0x100000

static int host_map_flags(int flags)
{
	int result = 0;

	if (flags & LINUX_MAP_SHARED)
		result |= MAP_SHARED;
	if (flags & LINUX_MAP_PRIVATE)
		result |= MAP_PRIVATE;
	if (flags & LINUX_MAP_ANONYMOUS)
		result |= MAP_ANON;
	return result;
}

long host_guest_mmap(uint64_t address, uint64_t size, int protection, int flags, int fd, int64_t offset)
{
	uint64_t length = round_up(size);
	int host_flags = host_map_flags(flags) | MAP_FIXED;
	void *result;

	if (!length)
		return -22; /* EINVAL */
	if (flags & LINUX_MAP_ANONYMOUS)
		fd = -1;
	if (flags & (LINUX_MAP_FIXED | LINUX_MAP_FIXED_NOREPLACE))
	{
		/* in the window, fresh memory (a block allocated, with its
		protection, or freed, without access) */
		if ((flags & LINUX_MAP_ANONYMOUS) && in_range(address, size, window_base, window_end))
		{
			window_refresh(address, size);
			return (long)address;
		}
		if (address + length > REGION_SIZE || (address & (PAGE - 1)) || !host_low_owns(address, length))
			return -12; /* ENOMEM */
		result = mmap(GUEST(void *, address), length, protection, host_flags, fd, offset);
		if (result == MAP_FAILED)
			return -host_linux_errno(errno);
		return (long)address;
	}
	address = take(length);
	if (!address)
		return -12;
	result = mmap(GUEST(void *, address), length, protection, host_flags, fd, offset);
	if (result == MAP_FAILED)
	{
		int error = errno;

		give_back(address, length);
		return -host_linux_errno(error);
	}
	return (long)address;
}

long host_guest_munmap(uint64_t address, uint64_t size)
{
	uint64_t length = round_up(size);

	if (address + length > REGION_SIZE)
		return -22;
	if (in_range(address, size, window_base, window_end))
	{
		window_refresh(address, size);
		return 0;
	}
	if (in_range(address, length, image_base, image_end))
		return -22;
	if (host_low_owns(address, length))
	{
		give_back(address, length);
		return 0;
	}
	return -22;
}

/* The guest protects 4 KB pages (the Xbox's); a host page may be bigger. A
host page only partly in the range keeps its access, unless the range gives
more of it (read and write, from read only) than it had: the other blocks
in the page must stay usable. */
long host_guest_mprotect(uint64_t address, uint64_t size, int protection)
{
	uint64_t start = address & ~(PAGE - 1);
	uint64_t end = round_up(address + size);
	uint64_t inner_start = round_up(address), inner_end = (address + size) & ~(PAGE - 1);

	if (address + size > REGION_SIZE || !size)
		return address + size > REGION_SIZE ? -22 : 0;
	if ((protection & (PROT_READ | PROT_WRITE)) == (PROT_READ | PROT_WRITE) || (address == start && address + size == end))
		return mprotect(GUEST(void *, start), end - start, protection) ? -host_linux_errno(errno) : 0;
	if (inner_start < inner_end && mprotect(GUEST(void *, inner_start), inner_end - inner_start, protection) != 0)
		return -host_linux_errno(errno);
	return 0;
}

/* ---------- write tracking (port/linux/src/memory_watch.c), in host pages */

#define WATCH_PAGE_COUNT (HALO_GUEST_WINDOW_SIZE / PAGE)

static uint8_t page_protected[WATCH_PAGE_COUNT];
static uint32_t page_generation[WATCH_PAGE_COUNT];
static volatile uint32_t current_generation = 1;
static int watch_active;

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
	mprotect(GUEST(void *, HALO_GUEST_WINDOW_BASE + page * PAGE), PAGE, PROT_READ | PROT_WRITE);
}

static struct sigaction previous_segv, previous_bus, previous_ill;

static void report_crash(int signal_number, siginfo_t *information, void *context)
{
	ucontext_t *ucontext = context;
	uint64_t pc, fp;
	int index;

#ifdef __aarch64__
	const _STRUCT_ARM_THREAD_STATE64 *registers = &ucontext->uc_mcontext->__ss;

	pc = registers->__pc;
	fp = registers->__fp;
	host_logf(HOST_LOG_ERROR, "signal %d at address %p: pc %016llx lr %016llx sp %016llx",
		signal_number, information->si_addr, (unsigned long long)pc, (unsigned long long)registers->__lr,
		(unsigned long long)registers->__sp);
	for (index = 0; index < 29; index += 4)
	{
		host_logf(HOST_LOG_ERROR, "  x%-2d %016llx %016llx %016llx %016llx", index,
			registers->__x[index], index + 1 < 29 ? registers->__x[index + 1] : 0,
			index + 2 < 29 ? registers->__x[index + 2] : 0, index + 3 < 29 ? registers->__x[index + 3] : 0);
	}
#else
	const _STRUCT_X86_THREAD_STATE64 *registers = &ucontext->uc_mcontext->__ss;

	pc = registers->__rip;
	fp = registers->__rbp;
	host_logf(HOST_LOG_ERROR, "signal %d at address %p: rip %016llx rsp %016llx rbp %016llx",
		signal_number, information->si_addr, (unsigned long long)pc, (unsigned long long)registers->__rsp,
		(unsigned long long)fp);
	host_logf(HOST_LOG_ERROR, "  rax %016llx rbx %016llx rcx %016llx rdx %016llx", registers->__rax, registers->__rbx,
		registers->__rcx, registers->__rdx);
	host_logf(HOST_LOG_ERROR, "  rsi %016llx rdi %016llx r8  %016llx r9  %016llx", registers->__rsi, registers->__rdi,
		registers->__r8, registers->__r9);
#endif
	if (pc >= host_guest_base + host_image.base && pc < host_guest_base + host_image.end)
		host_logf(HOST_LOG_ERROR, "  in the guest image: llvm-symbolizer --obj=build/macos*/Halo/halo_guest.elf 0x%llx",
			(unsigned long long)(pc - host_guest_base));
	/* the guest's frame records: saved frame pointer and return address */
	for (index = 0; index < 24 && fp >= host_guest_base && fp < host_guest_base + REGION_SIZE && (fp & 7) == 0; index++)
	{
		const uint64_t *frame = (const uint64_t *)(uintptr_t)fp;

		if (!host_low_owns(fp - host_guest_base, 16))
			break;
		host_logf(HOST_LOG_ERROR, "  frame %2d: return %016llx (guest %08llx)", index, (unsigned long long)frame[1],
			(unsigned long long)(uint32_t)frame[1]);
		fp = host_guest_base + (uint32_t)frame[0];
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
}

static int watched_write(siginfo_t *information)
{
	uint64_t host = (uint64_t)(uintptr_t)information->si_addr;
	uint64_t address;

	if (!watch_active || host < host_guest_base || host - host_guest_base >= REGION_SIZE)
		return 0;
	address = host - host_guest_base;
	if (in_window(address))
	{
		uint64_t page = watch_page(address);

		if (page_protected[page])
		{
			mark_written(page);
			return 1;
		}
	}
	return 0;
}

static void segv_handler(int signal_number, siginfo_t *information, void *context)
{
	if (watched_write(information))
		return;
	report_crash(signal_number, information, context);
	chain(&previous_segv, signal_number, information, context);
}

static void bus_handler(int signal_number, siginfo_t *information, void *context)
{
	if (watched_write(information))
		return;
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
	action.sa_flags = SA_SIGINFO | SA_NODEFER;
	sigemptyset(&action.sa_mask);
	action.sa_sigaction = segv_handler;
	sigaction(SIGSEGV, &action, &previous_segv);
	action.sa_sigaction = bus_handler;
	sigaction(SIGBUS, &action, &previous_bus);
	action.sa_sigaction = ill_handler;
	sigaction(SIGILL, &action, &previous_ill);
	/* a closed network connection must not end the game */
	signal(SIGPIPE, SIG_IGN);
}

void host_memory_watch_initialize(void)
{
	watch_active = 1;
}

void host_memory_watch_protect(uint32_t address, uint32_t size)
{
	uint64_t first, last, page;

	if (!watch_active || !size || !in_window(address))
		return;
	first = watch_page(address);
	last = watch_page((uint64_t)address + size - 1);
	if (last >= WATCH_PAGE_COUNT)
		last = WATCH_PAGE_COUNT - 1;
	for (page = first; page <= last; page++)
	{
		if (!page_protected[page])
		{
			page_protected[page] = 1;
			mprotect(GUEST(void *, HALO_GUEST_WINDOW_BASE + page * PAGE), PAGE, PROT_READ);
		}
	}
}

uint32_t host_memory_watch_serial(void)
{
	return current_generation;
}

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
	for (page = first; page <= last; page++)
	{
		if (page_generation[page] > newest)
			newest = page_generation[page];
	}
	return newest;
}

void host_memory_watch_prepare_write(uint32_t address, uint32_t size)
{
	uint64_t start = address, first, last, page;

	if (!watch_active || !size)
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

void host_memory_watch_forget(uint32_t address, uint32_t size)
{
	uint64_t first, last, page;

	if (!size || !in_window(address))
		return;
	first = watch_page(address);
	last = watch_page((uint64_t)address + size - 1);
	if (last >= WATCH_PAGE_COUNT)
		last = WATCH_PAGE_COUNT - 1;
	for (page = first; page <= last; page++)
	{
		/* writable again: the host page may hold other blocks too, which
		an operation on part of it would find read-only */
		if (page_protected[page])
			mprotect(GUEST(void *, HALO_GUEST_WINDOW_BASE + page * PAGE), PAGE, PROT_READ | PROT_WRITE);
		page_protected[page] = 0;
		page_generation[page] = __sync_add_and_fetch(&current_generation, 1);
	}
}
