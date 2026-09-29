#include "guest_allocator.h"

#include "guest_address.h"

#include <pthread.h>
#include <stdint.h>

static struct mac_guest_allocator guest_allocator;
static int guest_allocator_installed;
static pthread_mutex_t allocator_mutex = PTHREAD_MUTEX_INITIALIZER;

static int valid_alignment(size_t alignment)
{
	return alignment && (alignment & (alignment - 1)) == 0;
}

int mac_guest_allocator_install(const struct mac_guest_allocator *allocator)
{
	int result = -1;

	if (!allocator || !allocator->allocate || !allocator->release)
		return -1;
	pthread_mutex_lock(&allocator_mutex);
	if (!guest_allocator_installed)
	{
		guest_allocator = *allocator;
		guest_allocator_installed = 1;
		result = 0;
	}
	pthread_mutex_unlock(&allocator_mutex);
	return result;
}

int mac_guest_allocate(size_t size, size_t alignment, uint32_t *guest_va_out)
{
	struct mac_guest_allocator allocator;
	uint32_t guest_va = 0;
	int result;

	if (!size || !guest_va_out || !valid_alignment(alignment))
		return -1;
	pthread_mutex_lock(&allocator_mutex);
	if (!guest_allocator_installed)
	{
		pthread_mutex_unlock(&allocator_mutex);
		return -1;
	}
	allocator = guest_allocator;
	pthread_mutex_unlock(&allocator_mutex);
	result = allocator.allocate(allocator.context, size, alignment, &guest_va);
	if (result != 0)
		return result;
	if (!guest_va || (guest_va & (alignment - 1)) != 0 ||
		!mac_guest_address_resolve(guest_va, size))
	{
		allocator.release(allocator.context, guest_va, size);
		return -1;
	}
	*guest_va_out = guest_va;
	return 0;
}

int mac_guest_release(uint32_t guest_va, size_t size)
{
	struct mac_guest_allocator allocator;

	if (!guest_va || !size || !mac_guest_address_resolve(guest_va, size))
		return -1;
	pthread_mutex_lock(&allocator_mutex);
	if (!guest_allocator_installed)
	{
		pthread_mutex_unlock(&allocator_mutex);
		return -1;
	}
	allocator = guest_allocator;
	pthread_mutex_unlock(&allocator_mutex);
	allocator.release(allocator.context, guest_va, size);
	return 0;
}
