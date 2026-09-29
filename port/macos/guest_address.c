#include "guest_address.h"

#include <pthread.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

#define GUEST_ADDRESS_RANGE_LIMIT 64
#define GUEST_ADDRESS_SPACE_SIZE (UINT64_C(1) << 32)

struct guest_address_range
{
	uint32_t guest_base;
	uint64_t size;
	uintptr_t host_base;
	int owns_mapping;
	int occupied;
};

static struct guest_address_range ranges[GUEST_ADDRESS_RANGE_LIMIT];
static pthread_mutex_t ranges_lock = PTHREAD_MUTEX_INITIALIZER;
static void *resolve_guest_address(uint32_t guest_address, size_t size,
	int allow_null_address);

size_t mac_guest_address_host_page_size(void)
{
	long page_size = sysconf(_SC_PAGESIZE);
	return page_size > 0 ? (size_t)page_size : 0;
}

static int span_end(uint64_t base, size_t size, uint64_t *end_out)
{
	uint64_t end = base + (uint64_t)size;

	if (end < base)
		return -1;
	*end_out = end;
	return 0;
}

static int host_page_span(uint32_t guest_address, size_t size,
	uint32_t *aligned_address_out, size_t *aligned_size_out)
{
	size_t page_size = mac_guest_address_host_page_size();
	uint64_t end, aligned_end;
	uint64_t mask;

	if (!page_size || (page_size & (page_size - 1)) != 0 || !size ||
		span_end(guest_address, size, &end) != 0 || end > GUEST_ADDRESS_SPACE_SIZE)
		return -1;
	mask = (uint64_t)page_size - 1;
	aligned_end = (end + mask) & ~mask;
	if (aligned_end > GUEST_ADDRESS_SPACE_SIZE || aligned_end <= (guest_address & ~mask))
		return -1;
	*aligned_address_out = (uint32_t)((uint64_t)guest_address & ~mask);
	*aligned_size_out = (size_t)(aligned_end - *aligned_address_out);
	return 0;
}

int mac_guest_address_register(uint32_t guest_base, void *host_base, size_t size)
{
	uint64_t guest_end;
	uintptr_t host_start = (uintptr_t)host_base;
	size_t index, free_index = GUEST_ADDRESS_RANGE_LIMIT;
	int result = -1;

	if (!host_base || !size || span_end(guest_base, size, &guest_end) != 0 ||
		guest_end > GUEST_ADDRESS_SPACE_SIZE || size > UINTPTR_MAX - host_start)
		return -1;

	pthread_mutex_lock(&ranges_lock);
	for (index = 0; index < GUEST_ADDRESS_RANGE_LIMIT; index++)
	{
		const struct guest_address_range *range = &ranges[index];
		uint64_t range_guest_end;

		if (!range->occupied)
		{
			if (free_index == GUEST_ADDRESS_RANGE_LIMIT)
				free_index = index;
			continue;
		}
		range_guest_end = (uint64_t)range->guest_base + range->size;
		if ((uint64_t)guest_base < range_guest_end && range->guest_base < guest_end)
			goto done;
	}
	if (free_index != GUEST_ADDRESS_RANGE_LIMIT)
	{
		ranges[free_index].guest_base = guest_base;
		ranges[free_index].size = size;
		ranges[free_index].host_base = host_start;
		ranges[free_index].occupied = 1;
		result = 0;
	}

done:
	pthread_mutex_unlock(&ranges_lock);
	return result;
}

void *mac_guest_address_reserve(uint32_t guest_base, size_t size)
{
	void *host_base;
	size_t index;

	if (!size)
		return NULL;
	host_base = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (host_base == MAP_FAILED)
		return NULL;
	if (mac_guest_address_register(guest_base, host_base, size) != 0)
	{
		munmap(host_base, size);
		return NULL;
	}
	pthread_mutex_lock(&ranges_lock);
	for (index = 0; index < GUEST_ADDRESS_RANGE_LIMIT; index++)
	{
		if (ranges[index].occupied && ranges[index].guest_base == guest_base)
		{
			ranges[index].owns_mapping = 1;
			break;
		}
	}
	pthread_mutex_unlock(&ranges_lock);
	return host_base;
}

int mac_guest_address_commit(uint32_t guest_address, size_t size, int protection)
{
	void *host_address;
	size_t page_size = mac_guest_address_host_page_size();
	size_t aligned_size;
	uint32_t aligned_address;

	if (!page_size || host_page_span(guest_address, size, &aligned_address, &aligned_size) != 0 ||
		!(host_address = resolve_guest_address(aligned_address, aligned_size, 1)) ||
		(uintptr_t)host_address % page_size)
		return -1;
	return mprotect(host_address, aligned_size, protection);
}

int mac_guest_address_protect(uint32_t guest_address, size_t size, int protection)
{
	void *host_address;
	size_t page_size = mac_guest_address_host_page_size();
	size_t aligned_size;
	uint32_t aligned_address;

	if (!page_size || host_page_span(guest_address, size, &aligned_address, &aligned_size) != 0 ||
		!(host_address = resolve_guest_address(aligned_address, aligned_size, 1)) ||
		(uintptr_t)host_address % page_size)
		return -1;
	return mprotect(host_address, aligned_size, protection);
}

int mac_guest_address_unregister(uint32_t guest_base)
{
	size_t index;
	int result = -1;

	pthread_mutex_lock(&ranges_lock);
	for (index = 0; index < GUEST_ADDRESS_RANGE_LIMIT; index++)
	{
		if (ranges[index].occupied && ranges[index].guest_base == guest_base)
		{
			if (ranges[index].owns_mapping)
				munmap((void *)ranges[index].host_base, (size_t)ranges[index].size);
			ranges[index] = (struct guest_address_range){ 0 };
			result = 0;
			break;
		}
	}
	pthread_mutex_unlock(&ranges_lock);
	return result;
}

static void *resolve_guest_address(uint32_t guest_address, size_t size,
	int allow_null_address)
{
	uint64_t guest_end;
	void *result = NULL;
	size_t index;

	if ((!allow_null_address && !guest_address) || span_end(guest_address, size, &guest_end) != 0 ||
		guest_end > GUEST_ADDRESS_SPACE_SIZE)
		return NULL;
	pthread_mutex_lock(&ranges_lock);
	for (index = 0; index < GUEST_ADDRESS_RANGE_LIMIT; index++)
	{
		const struct guest_address_range *range = &ranges[index];
		uint64_t range_end;

		if (!range->occupied)
			continue;
		range_end = (uint64_t)range->guest_base + range->size;
		if (guest_address >= range->guest_base && guest_end <= range_end)
		{
			result = (void *)(range->host_base + (guest_address - range->guest_base));
			break;
		}
	}
	pthread_mutex_unlock(&ranges_lock);
	return result;
}

void *mac_guest_address_resolve(uint32_t guest_address, size_t size)
{
	return resolve_guest_address(guest_address, size, 0);
}

size_t mac_guest_address_available(uint32_t guest_address)
{
	size_t available = 0;
	size_t index;

	if (!guest_address)
		return 0;
	pthread_mutex_lock(&ranges_lock);
	for (index = 0; index < GUEST_ADDRESS_RANGE_LIMIT; index++)
	{
		const struct guest_address_range *range = &ranges[index];
		uint64_t range_end;

		if (!range->occupied || guest_address < range->guest_base)
			continue;
		range_end = (uint64_t)range->guest_base + range->size;
		if (guest_address < range_end)
		{
			uint64_t remaining = range_end - guest_address;
			available = remaining > SIZE_MAX ? SIZE_MAX : (size_t)remaining;
			break;
		}
	}
	pthread_mutex_unlock(&ranges_lock);
	return available;
}

int mac_guest_address_from_host(const void *host_address, size_t size,
	uint32_t *guest_address_out)
{
	uintptr_t host_start = (uintptr_t)host_address;
	uintptr_t host_end;
	size_t index;
	int found = 0;
	int result = -1;

	if (!host_address || !guest_address_out || size > UINTPTR_MAX - host_start)
		return -1;
	host_end = host_start + size;
	pthread_mutex_lock(&ranges_lock);
	for (index = 0; index < GUEST_ADDRESS_RANGE_LIMIT; index++)
	{
		const struct guest_address_range *range = &ranges[index];
		uintptr_t range_end;
		uint64_t guest_address;

		if (!range->occupied)
			continue;
		range_end = range->host_base + (uintptr_t)range->size;
		if (host_start < range->host_base || host_end > range_end)
			continue;
		guest_address = (uint64_t)range->guest_base + (host_start - range->host_base);
		if (guest_address == 0 || guest_address > UINT32_MAX)
			break;
		if (found)
		{
			result = -1;
			found = 0;
			break;
		}
		*guest_address_out = (uint32_t)guest_address;
		found = 1;
	}
	if (found)
		result = 0;
	pthread_mutex_unlock(&ranges_lock);
	return result;
}

int mac_guest_address_linear_offset(uint32_t guest_base, const void *host_base,
	ptrdiff_t *offset_out)
{
	size_t index;
	int result = -1;
	uintptr_t host = (uintptr_t)host_base;

	if (!host_base || !offset_out || host > (uintptr_t)PTRDIFF_MAX ||
		(uintptr_t)guest_base > (uintptr_t)PTRDIFF_MAX)
		return -1;
	pthread_mutex_lock(&ranges_lock);
	for (index = 0; index < GUEST_ADDRESS_RANGE_LIMIT; index++)
	{
		if (ranges[index].occupied && ranges[index].guest_base == guest_base &&
			ranges[index].host_base == host)
		{
			*offset_out = (ptrdiff_t)host - (ptrdiff_t)guest_base;
			result = 0;
			break;
		}
	}
	pthread_mutex_unlock(&ranges_lock);
	return result;
}

void mac_guest_address_reset(void)
{
	pthread_mutex_lock(&ranges_lock);
	for (size_t index = 0; index < GUEST_ADDRESS_RANGE_LIMIT; index++)
	{
		if (ranges[index].occupied && ranges[index].owns_mapping)
			munmap((void *)ranges[index].host_base, (size_t)ranges[index].size);
		ranges[index] = (struct guest_address_range){ 0 };
	}
	pthread_mutex_unlock(&ranges_lock);
}
