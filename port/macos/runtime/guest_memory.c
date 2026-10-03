#include "guest_memory.h"

#include <mach/mach_vm.h>
#include <unistd.h>

kern_return_t halo_macos_memory_create(struct halo_macos_memory *memory)
{
	mach_vm_address_t base = 0;
	kern_return_t result;
	long page_size;

	if (!memory || memory->base || memory->page_size)
		return KERN_INVALID_ARGUMENT;
	page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0)
		return KERN_FAILURE;
	/* The alignment mask requests a 4 GB-aligned free range. ANYWHERE does
	   not overwrite an existing mapping, unlike a fixed mmap replacement. */
	result = mach_vm_map(mach_task_self(), &base, HALO_MACOS_ADDRESS_SPACE_SIZE,
		HALO_MACOS_ADDRESS_SPACE_SIZE - 1, VM_FLAGS_ANYWHERE,
		MEMORY_OBJECT_NULL, 0, FALSE, VM_PROT_NONE,
		VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE);
	if (result != KERN_SUCCESS)
		return result;
	if (base < HALO_MACOS_ADDRESS_SPACE_SIZE ||
		base % HALO_MACOS_ADDRESS_SPACE_SIZE != 0 ||
		base > UINTPTR_MAX - HALO_MACOS_ADDRESS_SPACE_SIZE)
	{
		mach_vm_deallocate(mach_task_self(), base, HALO_MACOS_ADDRESS_SPACE_SIZE);
		return KERN_NO_SPACE;
	}
	memory->base = (uintptr_t)base;
	memory->page_size = (size_t)page_size;
	return KERN_SUCCESS;
}

kern_return_t halo_macos_memory_destroy(struct halo_macos_memory *memory)
{
	kern_return_t result;

	if (!memory || !memory->base)
		return KERN_INVALID_ARGUMENT;
	result = mach_vm_deallocate(mach_task_self(), memory->base,
		HALO_MACOS_ADDRESS_SPACE_SIZE);
	if (result == KERN_SUCCESS)
	{
		memory->base = 0;
		memory->page_size = 0;
	}
	return result;
}

void *halo_macos_guest_pointer(const struct halo_macos_memory *memory,
	uint32_t address, uint64_t size)
{
	if (!memory || !memory->base || !address || !size ||
		size > HALO_MACOS_ADDRESS_SPACE_SIZE - address)
		return NULL;
	return (void *)(memory->base + address);
}

int halo_macos_guest_address(const struct halo_macos_memory *memory,
	const void *pointer, uint32_t *address)
{
	uintptr_t value = (uintptr_t)pointer;

	if (!memory || !memory->base || !address || value <= memory->base ||
		value - memory->base >= HALO_MACOS_ADDRESS_SPACE_SIZE)
		return 0;
	*address = (uint32_t)(value - memory->base);
	return 1;
}

kern_return_t halo_macos_memory_protect(const struct halo_macos_memory *memory,
	uint32_t address, uint64_t size, vm_prot_t protection)
{
	void *pointer = halo_macos_guest_pointer(memory, address, size);

	if (!pointer || !memory->page_size || address % memory->page_size ||
		size % memory->page_size ||
		(protection & ~(VM_PROT_READ | VM_PROT_WRITE)))
		return KERN_INVALID_ARGUMENT;
	return mach_vm_protect(mach_task_self(), (mach_vm_address_t)pointer,
		size, FALSE, protection);
}
