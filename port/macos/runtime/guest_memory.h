/* Preliminary Darwin host memory contract; no game ABI conversion yet. */
#ifndef HALO_MACOS_GUEST_MEMORY_H
#define HALO_MACOS_GUEST_MEMORY_H

#include <mach/mach.h>
#include <stddef.h>
#include <stdint.h>

#define HALO_MACOS_ADDRESS_SPACE_SIZE (UINT64_C(1) << 32)

struct halo_macos_memory
{
	uintptr_t base;
	size_t page_size;
};

/* Zero-initialize before use. Ownership is exclusive; do not copy a live
   instance or destroy it while another thread uses it. No executable pages. */
kern_return_t halo_macos_memory_create(struct halo_macos_memory *memory);
kern_return_t halo_macos_memory_destroy(struct halo_macos_memory *memory);

/* Zero is the guest NULL pointer. Translation checks bounds, not accessibility.
   A translated host pointer must never be stored in a 32-bit game field. */
void *halo_macos_guest_pointer(const struct halo_macos_memory *memory,
	uint32_t address, uint64_t size);
int halo_macos_guest_address(const struct halo_macos_memory *memory,
	const void *pointer, uint32_t *address);

/* Require exact host-page boundaries: never silently widen a 4 KB game
   request to a 16 KB host page and change a neighbouring block's protection. */
kern_return_t halo_macos_memory_protect(const struct halo_macos_memory *memory,
	uint32_t address, uint64_t size, vm_prot_t protection);

#endif
