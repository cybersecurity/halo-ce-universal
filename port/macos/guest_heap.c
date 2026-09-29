#include "guest_heap.h"

#include "guest_address.h"
#include "guest_allocator.h"
#include "host_syscall.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define IMPORT_BASE UINT32_C(0x70000000)
#define GUEST_SPACE_SIZE (UINT64_C(1) << 32)
#define IMPORT_RESERVED_SIZE UINT32_C(0x10000)
#define GUEST_PAGE_SIZE UINT32_C(4096)
#define LINUX_MAP_FIXED UINT32_C(0x10)
#define LINUX_MAP_ANONYMOUS UINT32_C(0x20)
#define LINUX_MAP_NORESERVE UINT32_C(0x4000)
#define LINUX_MAP_FIXED_NOREPLACE UINT32_C(0x100000)

struct guest_heap_state
{
	pthread_mutex_t mutex;
	uint32_t base;
	uint32_t page_size;
	uint32_t page_count;
	uint32_t total_page_count;
	uint8_t *allocated;
	uint8_t *reserved;
	uint8_t *replaceable;
	uint8_t *protection;
	/* Native protection granularity can be 16KB while Xbox mappings are 4KB. */
	uint8_t *guest_protection;
	int installed;
};

static struct guest_heap_state heap = { .mutex = PTHREAD_MUTEX_INITIALIZER };

static int round_guest_span(size_t size, uint32_t *pages_out, uint32_t *rounded_out)
{
	uint64_t pages;
	if (!size || !heap.page_size)
		return -EINVAL;
	pages = ((uint64_t)size + heap.page_size - 1) / heap.page_size;
	if (!pages || pages > heap.total_page_count || pages * heap.page_size > UINT32_MAX)
		return -ENOMEM;
	*pages_out = (uint32_t)pages;
	*rounded_out = (uint32_t)(pages * heap.page_size);
	return 0;
}

static int find_free_pages(uint32_t requested_va, uint32_t page_count,
	uint32_t first_limit, uint32_t page_alignment, int fixed,
	uint32_t *first_page_out)
{
	uint32_t first = 0;
	if (fixed)
	{
		first = requested_va / heap.page_size;
		if (first > heap.total_page_count || page_count > heap.total_page_count - first)
			return -ENOMEM;
		for (uint32_t i = 0; i < page_count; ++i)
			if (heap.allocated[first + i])
				return -ENOMEM;
		*first_page_out = first;
		return 0;
	}
	uint32_t first_available = heap.base / heap.page_size;
	if (!page_alignment || (page_alignment & (page_alignment - 1)) ||
		page_alignment % heap.page_size)
		return -EINVAL;
	if (first_limit < first_available || page_count > first_limit - first_available)
		return -ENOMEM;
	for (first = first_available;
		first <= first_limit - page_count; ++first)
	{
		uint32_t i;
		uint64_t address = (uint64_t)first * heap.page_size;
		if (address & (page_alignment - 1))
			continue;
		for (i = 0; i < page_count && !heap.allocated[first + i]; ++i) {}
		if (i == page_count)
		{
			*first_page_out = first;
			return 0;
		}
		first += i;
	}
	return -ENOMEM;
}

static int commit_pages(uint32_t first, uint32_t count, int protection)
{
	uint32_t address = first * heap.page_size;
	size_t length = (size_t)count * heap.page_size;
	if (mac_guest_address_commit(address, length, protection) != 0)
		return -ENOMEM;
	return 0;
}

static int range_is_replaceable_reservation(uint32_t first, uint32_t count)
{
	if (first > heap.total_page_count || count > heap.total_page_count - first)
		return 0;
	for (uint32_t i = 0; i < count; ++i)
		if (!heap.replaceable[first + i])
			return 0;
	return 1;
}

static uint32_t guest_round_4k(uint32_t size)
{
	return (uint32_t)(((uint64_t)size + GUEST_PAGE_SIZE - 1) &
		~(uint64_t)(GUEST_PAGE_SIZE - 1));
}

static int is_replaceable_guest_span(uint32_t address, uint32_t length)
{
	uint64_t end = (uint64_t)address + length;
	uint32_t first, last;
	if (!length || address % GUEST_PAGE_SIZE || end > GUEST_SPACE_SIZE ||
		end % GUEST_PAGE_SIZE)
		return 0;
	first = address / heap.page_size;
	last = (uint32_t)((end + heap.page_size - 1) / heap.page_size);
	return range_is_replaceable_reservation(first, last - first);
}

static int set_guest_span_protection(uint32_t address, uint32_t length,
	int protection)
{
	uint32_t guest_first = address / GUEST_PAGE_SIZE;
	uint32_t guest_count = length / GUEST_PAGE_SIZE;
	uint32_t host_first = address / heap.page_size;
	uint32_t host_last = (uint32_t)(((uint64_t)address + length + heap.page_size - 1) /
		heap.page_size);
	for (uint32_t i = 0; i < guest_count; ++i)
		heap.guest_protection[guest_first + i] = (uint8_t)protection;
	/* A 16KB host page receives the union of its four guest-page permissions.
	 * macOS cannot enforce independent protection within those subpages, but
	 * this preserves neighboring data and avoids making a live block inaccessible. */
	for (uint32_t hp = host_first; hp < host_last; ++hp)
	{
		uint32_t gf = hp * heap.page_size / GUEST_PAGE_SIZE;
		int effective = PROT_NONE;
		for (uint32_t sub = 0; sub < heap.page_size / GUEST_PAGE_SIZE; ++sub)
			effective |= heap.guest_protection[gf + sub];
		if (mac_guest_address_protect(hp * heap.page_size, heap.page_size,
			effective) != 0)
			return -1;
		heap.protection[hp] = (uint8_t)effective;
	}
	return 0;
}

static int release_pages(uint32_t first, uint32_t count)
{
	uint32_t address = first * heap.page_size;
	size_t length = (size_t)count * heap.page_size;
	if (mac_guest_address_protect(address, length, PROT_NONE) != 0)
		return -EINVAL;
	/* The next mmap/stack allocation must see zero-filled pages, as it would
	 * when the kernel first maps anonymous memory. */
	void *host = mac_guest_address_resolve(address, length);
	if (!host || madvise(host, length, MADV_DONTNEED) != 0)
		return -ENOMEM;
	for (uint32_t i = 0; i < count; ++i)
	{
		heap.allocated[first + i] = 0;
		heap.protection[first + i] = 0;
	}
	return 0;
}

static int heap_allocate(void *context, size_t size, size_t alignment,
	uint32_t *guest_va_out)
{
	uint32_t count, rounded, first;
	int result;
	(void)context;
	if (!guest_va_out || !alignment || (alignment & (alignment - 1)))
		return EINVAL;
	if ((result = round_guest_span(size, &count, &rounded)) != 0)
		return -result;
	if (alignment < heap.page_size)
		alignment = heap.page_size;
	if (alignment > UINT32_MAX || alignment % heap.page_size)
		return EINVAL;
	pthread_mutex_lock(&heap.mutex);
	result = find_free_pages(0, count, heap.base / heap.page_size + heap.page_count,
		(uint32_t)alignment, 0, &first);
	if (!result)
	{
		result = commit_pages(first, count, PROT_READ | PROT_WRITE);
		if (!result)
		{
			for (uint32_t i = 0; i < count; ++i)
			{
				heap.allocated[first + i] = 1;
				heap.protection[first + i] = PROT_READ | PROT_WRITE;
			}
			memset(mac_guest_address_resolve(first * heap.page_size, rounded),
				0, rounded);
			*guest_va_out = first * heap.page_size;
		}
	}
	pthread_mutex_unlock(&heap.mutex);
	return result > 0 ? result : -result;
}

static void heap_release(void *context, uint32_t guest_va, size_t size)
{
	uint32_t count, rounded;
	uint32_t offset;
	int result;
	(void)context;
	if (!heap.installed || guest_va < heap.base)
		return;
	offset = guest_va - heap.base;
	if (offset % heap.page_size || round_guest_span(size, &count, &rounded) != 0)
		return;
	pthread_mutex_lock(&heap.mutex);
	uint32_t first = guest_va / heap.page_size;
	if (first <= heap.total_page_count && count <= heap.total_page_count - first)
	{
		result = 0;
		for (uint32_t i = 0; i < count; ++i)
			if (!heap.allocated[first + i] || heap.reserved[first + i])
			{ result = -EINVAL; break; }
		if (!result)
			result = release_pages(first, count);
		(void)result;
	}
	pthread_mutex_unlock(&heap.mutex);
}

static int64_t vm_map(void *context, uint32_t requested, uint32_t size,
	int protection, uint32_t flags, int fd, uint64_t offset,
	uint32_t *mapped_out)
{
	uint32_t count, rounded, first, map_page_offset = 0;
	int no_replace = (flags & LINUX_MAP_FIXED_NOREPLACE) != 0;
	int fixed = (flags & LINUX_MAP_FIXED) != 0 || no_replace;
	int result;
	(void)context;
	if (!mapped_out || !size || fd != -1 || offset != 0 ||
		!(flags & LINUX_MAP_ANONYMOUS) || !(flags & UINT32_C(0x02)))
		return -38; /* ENOSYS: file mappings are not installed in the arena. */
	if (protection & PROT_EXEC)
		return -EACCES; /* guest code is translated; never expose executable arena pages. */
	if (fixed)
	{
		uint64_t end = (uint64_t)requested + size;
		if (requested % GUEST_PAGE_SIZE || end > GUEST_SPACE_SIZE)
			return -EINVAL;
		map_page_offset = requested % heap.page_size;
		count = (uint32_t)(((uint64_t)map_page_offset + size + heap.page_size - 1) /
			heap.page_size);
		if (count > heap.total_page_count)
			return -ENOMEM;
		rounded = count * heap.page_size;
	}
	else if ((result = round_guest_span(size, &count, &rounded)) != 0)
		return result;
	pthread_mutex_lock(&heap.mutex);
	if (fixed && !no_replace)
	{
		uint64_t first64 = (uint64_t)requested / heap.page_size;
		if (first64 > heap.total_page_count || count > heap.total_page_count - first64)
			result = -ENOMEM;
		else
		{
			first = (uint32_t)first64;
			/* MAP_FIXED may replace pages inside the reserved Xbox contiguous
			 * window. Outside that explicitly-owned range, refuse to clobber
			 * existing image/import/guest mappings. */
			if (range_is_replaceable_reservation(first, count))
				result = 0;
			else
				result = find_free_pages(requested, count, heap.total_page_count,
					heap.page_size, 1, &first);
		}
	}
	else
		result = find_free_pages(requested, count,
			fixed ? heap.total_page_count : heap.base / heap.page_size + heap.page_count,
			heap.page_size, fixed, &first);
	if (!result && no_replace && protection == PROT_NONE)
	{
		/* The source Xbox backend first reserves its 128 MiB physical window
		 * with PROT_NONE|MAP_FIXED_NOREPLACE|MAP_NORESERVE. The whole guest
		 * address space already has a Darwin PROT_NONE reservation, so this
		 * operation records ownership without touching/committing 128 MiB. */
		*mapped_out = requested;
		for (uint32_t i = 0; i < count; ++i)
		{
			heap.allocated[first + i] = 1;
			heap.reserved[first + i] = 1;
			heap.replaceable[first + i] = 1;
			heap.protection[first + i] = PROT_NONE;
		}
		if (is_replaceable_guest_span(requested, rounded))
			memset(heap.guest_protection + requested / GUEST_PAGE_SIZE, PROT_NONE,
				rounded / GUEST_PAGE_SIZE);
		pthread_mutex_unlock(&heap.mutex);
		return 0;
	}
	if (!result && fixed && !no_replace && protection == PROT_NONE &&
		is_replaceable_guest_span(requested, guest_round_4k(size)))
	{
		uint32_t guest_length = guest_round_4k(size);
		void *host = mac_guest_address_resolve(requested, guest_length);
		/* MAP_FIXED of anonymous PROT_NONE pages is the source backend's
		 * XPhysicalFree operation: discard the old backing while retaining the
		 * window reservation. This also guarantees future recommit is zeroed. */
		if (!host || commit_pages(first, count, PROT_READ | PROT_WRITE) != 0)
			result = -ENOMEM;
		else
		{
			/* Darwin MADV_DONTNEED may retain anonymous bytes until memory
			 * pressure, unlike replacing an anonymous Linux MAP_FIXED mapping.
			 * Clear explicitly before restoring the guard protection. */
			memset(host, 0, guest_length);
			if (set_guest_span_protection(requested, guest_length, PROT_NONE) != 0)
			{
				result = -ENOMEM;
				pthread_mutex_unlock(&heap.mutex);
				return result;
			}
			*mapped_out = requested;
			pthread_mutex_unlock(&heap.mutex);
			return 0;
		}
	}
	if (!result)
	{
		/* Anonymous mmap pages are cleared before the requested final protection
		 * takes effect. A read-only or PROT_NONE mapping must still be writable
		 * during this host-side initialization. */
		result = commit_pages(first, count, PROT_READ | PROT_WRITE);
		if (!result)
		{
			for (uint32_t i = 0; i < count; ++i)
			{
			heap.allocated[first + i] = 1;
			if (no_replace)
			{
				heap.reserved[first + i] = 1;
				heap.replaceable[first + i] = 1;
			}
			heap.protection[first + i] = (uint8_t)(PROT_READ | PROT_WRITE);
			}
			*mapped_out = fixed ? requested : first * heap.page_size;
			/* Linux zeroes the entire guest page span, including the tail of
			 * an unaligned mmap length. Darwin may retain released backing;
			 * musl mallocng relies on that tail for its zero end marker. Fixed
			 * 4KB subpage mappings must preserve neighboring Xbox blocks. */
			uint32_t clear_length = fixed ? guest_round_4k(size) : rounded;
			memset(mac_guest_address_resolve(*mapped_out, clear_length), 0, clear_length);
			if (fixed && is_replaceable_guest_span(*mapped_out, guest_round_4k(size)))
			{
				if (set_guest_span_protection(*mapped_out, guest_round_4k(size), protection) != 0)
					result = -ENOMEM;
			}
			else if (protection != (PROT_READ | PROT_WRITE) &&
				mac_guest_address_protect(first * heap.page_size,
					(size_t)count * heap.page_size, protection) != 0)
			{
				(void)release_pages(first, count);
				result = -ENOMEM;
			}
			else if (!result)
			{
				for (uint32_t i = 0; i < count; ++i)
					heap.protection[first + i] = (uint8_t)protection;
			}
		}
	}
	pthread_mutex_unlock(&heap.mutex);
	return result;
}

static int64_t vm_unmap(void *context, uint32_t address, uint32_t size)
{
	uint32_t count, first, offset;
	uint64_t end = (uint64_t)address + size;
	int result;
	(void)context;
	if (!size)
		return -EINVAL;
	if (!size || address % GUEST_PAGE_SIZE || end > GUEST_SPACE_SIZE)
		return -EINVAL;
	first = address / heap.page_size;
	offset = address % heap.page_size;
	count = (uint32_t)(((uint64_t)offset + size + heap.page_size - 1) /
		heap.page_size);
	pthread_mutex_lock(&heap.mutex);
	if (first > heap.total_page_count || count > heap.total_page_count - first)
		result = -EINVAL;
	else
	{
		for (uint32_t i = 0; i < count; ++i)
			if (!heap.allocated[first + i] || heap.reserved[first + i])
			{ pthread_mutex_unlock(&heap.mutex); return -EINVAL; }
		result = release_pages(first, count);
	}
	pthread_mutex_unlock(&heap.mutex);
	return result;
}

static int64_t vm_protect(void *context, uint32_t address, uint32_t size,
	int protection)
{
	uint32_t count, first, offset;
	uint64_t end = (uint64_t)address + size;
	int result = 0;
	(void)context;
	if (!size || protection & PROT_EXEC)
		return -EINVAL;
	if (!size || address % GUEST_PAGE_SIZE || end > GUEST_SPACE_SIZE)
		return -EINVAL;
	first = address / heap.page_size;
	offset = address % heap.page_size;
	count = (uint32_t)(((uint64_t)offset + size + heap.page_size - 1) /
		heap.page_size);
	pthread_mutex_lock(&heap.mutex);
	if (first > heap.total_page_count || count > heap.total_page_count - first)
		result = -EINVAL;
	else
	{
		for (uint32_t i = 0; i < count; ++i)
			if (!heap.allocated[first + i]) { result = -EINVAL; break; }
		if (!result && is_replaceable_guest_span(address, guest_round_4k(size)))
			result = set_guest_span_protection(address, guest_round_4k(size), protection);
		else if (!result && mac_guest_address_protect(first * heap.page_size,
			(size_t)count * heap.page_size, protection) != 0)
			result = -ENOMEM;
		if (!result)
			for (uint32_t i = 0; i < count; ++i) heap.protection[first + i] = (uint8_t)protection;
	}
	pthread_mutex_unlock(&heap.mutex);
	return result;
}

int mac_guest_heap_install(uint32_t image_end_va)
{
	struct mac_guest_allocator allocator = { NULL, heap_allocate, heap_release };
	struct mac_guest_vm_ops operations = { NULL, vm_map, vm_unmap, vm_protect };
	size_t host_page = mac_guest_address_host_page_size();
	uint64_t aligned_base;
	if (heap.installed || !host_page || (host_page & (host_page - 1)) ||
		image_end_va >= IMPORT_BASE || (IMPORT_BASE % host_page) ||
		(IMPORT_RESERVED_SIZE % host_page))
		return -1;
	aligned_base = ((uint64_t)image_end_va + host_page - 1) & ~((uint64_t)host_page - 1);
	if (aligned_base >= IMPORT_BASE || ((IMPORT_BASE - aligned_base) % host_page))
		return -1;
	heap.base = (uint32_t)aligned_base;
	heap.page_size = (uint32_t)host_page;
	heap.page_count = (IMPORT_BASE - heap.base) / heap.page_size;
	heap.total_page_count = (uint32_t)(GUEST_SPACE_SIZE / heap.page_size);
	heap.allocated = calloc(heap.total_page_count, 1);
	heap.reserved = calloc(heap.total_page_count, 1);
	heap.replaceable = calloc(heap.total_page_count, 1);
	heap.protection = calloc(heap.total_page_count, 1);
	heap.guest_protection = calloc(GUEST_SPACE_SIZE / GUEST_PAGE_SIZE, 1);
	if (!heap.allocated || !heap.reserved || !heap.replaceable || !heap.protection ||
		!heap.guest_protection)
	{
		free(heap.allocated); free(heap.reserved); free(heap.replaceable); free(heap.protection);
		free(heap.guest_protection);
		heap.allocated = heap.reserved = heap.replaceable = heap.protection = NULL;
		heap.guest_protection = NULL;
		return -1;
	}
	/* Keep null page, loaded image, and synthetic import-stub range unavailable
	 * to brk/mmap allocations. MAP_FIXED at 0x80000000 (the Xbox RAM window)
	 * remains legal when its pages are otherwise free. */
	for (uint32_t page = 0; page < (uint32_t)(((uint64_t)image_end_va + host_page - 1) / host_page); ++page)
		heap.allocated[page] = heap.reserved[page] = 1;
	{
		uint32_t first = IMPORT_BASE / heap.page_size;
		uint32_t count = IMPORT_RESERVED_SIZE / heap.page_size;
		for (uint32_t page = 0; page < count; ++page)
			heap.allocated[first + page] = heap.reserved[first + page] = 1;
		memset(heap.guest_protection + ((first * heap.page_size) / GUEST_PAGE_SIZE),
			PROT_NONE, (size_t)count * heap.page_size / GUEST_PAGE_SIZE);
	}
	if (mac_guest_allocator_install(&allocator) != 0 || mac_guest_vm_ops_install(&operations) != 0)
	{
		free(heap.allocated); free(heap.reserved); free(heap.replaceable); free(heap.protection);
		free(heap.guest_protection);
		heap.allocated = heap.reserved = heap.replaceable = heap.protection = NULL;
		heap.guest_protection = NULL;
		return -1;
	}
	heap.installed = 1;
	return 0;
}
