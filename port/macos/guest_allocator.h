#ifndef HALO_MACOS_GUEST_ALLOCATOR_H
#define HALO_MACOS_GUEST_ALLOCATOR_H

#include <stddef.h>
#include <stdint.h>

/* The platform layer must not pick guest VAs independently of the guest
 * heap/stack map. The executable installs its real allocator here; returned
 * spans must already be committed and registered with guest_address. */
struct mac_guest_allocator
{
	void *context;
	int (*allocate)(void *context, size_t size, size_t alignment,
		uint32_t *guest_va_out);
	void (*release)(void *context, uint32_t guest_va, size_t size);
};

int mac_guest_allocator_install(const struct mac_guest_allocator *allocator);
int mac_guest_allocate(size_t size, size_t alignment, uint32_t *guest_va_out);
int mac_guest_release(uint32_t guest_va, size_t size);

#endif
