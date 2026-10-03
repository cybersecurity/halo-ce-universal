#include "../runtime/guest_memory.h"
#include "../../android/include/halo_android_abi.h"

#include <mach/mach_vm.h>
#include <stdio.h>

#define CHECK(condition) do { if (!(condition)) { \
	fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
	goto failure; } } while (0)

static int protection_is(void *pointer, vm_prot_t expected)
{
	mach_vm_address_t address = (mach_vm_address_t)pointer;
	mach_vm_size_t size;
	vm_region_basic_info_data_64_t info;
	mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
	mach_port_t object = MACH_PORT_NULL;
	kern_return_t result = mach_vm_region(mach_task_self(), &address, &size,
		VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object);
	if (object != MACH_PORT_NULL)
		mach_port_deallocate(mach_task_self(), object);
	return result == KERN_SUCCESS && address <= (mach_vm_address_t)pointer &&
		info.protection == expected;
}

int main(void)
{
	struct halo_macos_memory memory = {0}, second = {0};
	uint32_t address = 0;
	volatile uint32_t *window;
	void *image;

	CHECK(halo_macos_memory_create(&memory) == KERN_SUCCESS);
	printf("arm64 host: pointers=%zu, page=%zu, guest base=0x%llx\n",
		sizeof(void *), memory.page_size, (unsigned long long)memory.base);
	CHECK(sizeof(void *) == 8);
	CHECK(memory.base % HALO_MACOS_ADDRESS_SPACE_SIZE == 0);
	CHECK(halo_macos_memory_create(&memory) == KERN_INVALID_ARGUMENT);
	CHECK(halo_macos_memory_create(&second) == KERN_SUCCESS);
	CHECK(second.base != memory.base);

	window = halo_macos_guest_pointer(&memory, HALO_GUEST_WINDOW_BASE,
		HALO_GUEST_WINDOW_SIZE);
	image = halo_macos_guest_pointer(&memory, HALO_GUEST_IMAGE_BASE, memory.page_size);
	CHECK(window && image);
	CHECK(protection_is((void *)window, VM_PROT_NONE));
	CHECK(halo_macos_memory_protect(&memory, HALO_GUEST_WINDOW_BASE,
		memory.page_size, VM_PROT_READ | VM_PROT_WRITE) == KERN_SUCCESS);
	window[0] = 0x48414c4f;
	window[memory.page_size / sizeof(*window) - 1] = 0x12345678;
	CHECK(window[0] == 0x48414c4f);
	CHECK(window[memory.page_size / sizeof(*window) - 1] == 0x12345678);
	CHECK(halo_macos_guest_address(&memory, (void *)window, &address));
	CHECK(address == HALO_GUEST_WINDOW_BASE);
	CHECK(!halo_macos_guest_pointer(&memory, 0, 1));
	CHECK(!halo_macos_guest_pointer(&memory, 1, 0));
	CHECK(!halo_macos_guest_pointer(&memory, UINT32_MAX, 2));
	CHECK(!halo_macos_guest_pointer(&memory, 1, UINT64_MAX));
	CHECK(halo_macos_guest_pointer(&memory, UINT32_MAX, 1));
	CHECK(halo_macos_guest_address(&memory,
		halo_macos_guest_pointer(&memory, UINT32_MAX, 1), &address));
	CHECK(address == UINT32_MAX);
	CHECK(!halo_macos_guest_address(&memory, NULL, &address));
	CHECK(!halo_macos_guest_address(&memory, (void *)memory.base, &address));
	CHECK(!halo_macos_guest_address(&memory,
		(void *)(memory.base + HALO_MACOS_ADDRESS_SPACE_SIZE), &address));
	CHECK(!halo_macos_guest_address(&memory, &memory, &address));
	CHECK(halo_macos_memory_protect(&memory, HALO_GUEST_WINDOW_BASE + 1,
		memory.page_size, VM_PROT_READ) == KERN_INVALID_ARGUMENT);
	if (memory.page_size > 4096)
		CHECK(halo_macos_memory_protect(&memory, HALO_GUEST_WINDOW_BASE,
			4096, VM_PROT_READ) == KERN_INVALID_ARGUMENT);
	CHECK(halo_macos_memory_protect(&memory, HALO_GUEST_WINDOW_BASE,
		memory.page_size, VM_PROT_EXECUTE) == KERN_INVALID_ARGUMENT);
	CHECK(halo_macos_memory_protect(&memory, HALO_GUEST_WINDOW_BASE,
		memory.page_size, VM_PROT_READ) == KERN_SUCCESS);
	CHECK(protection_is((void *)window, VM_PROT_READ));
	CHECK(protection_is((char *)window + memory.page_size, VM_PROT_NONE));
	CHECK(protection_is(image, VM_PROT_NONE));
	CHECK(window[0] == 0x48414c4f);
	CHECK(halo_macos_memory_protect(&memory, HALO_GUEST_WINDOW_BASE,
		memory.page_size, VM_PROT_NONE) == KERN_SUCCESS);
	CHECK(protection_is((void *)window, VM_PROT_NONE));
	CHECK(halo_macos_memory_destroy(&memory) == KERN_SUCCESS);
	CHECK(!halo_macos_guest_pointer(&memory, HALO_GUEST_WINDOW_BASE, 4));
	CHECK(halo_macos_memory_destroy(&second) == KERN_SUCCESS);
	CHECK(halo_macos_memory_create(&memory) == KERN_SUCCESS);
	CHECK(halo_macos_memory_destroy(&memory) == KERN_SUCCESS);
	puts("PASS: reservation, Xbox offsets, bounds, protection isolation, lifecycle");
	return 0;
failure:
	if (second.base) halo_macos_memory_destroy(&second);
	if (memory.base) halo_macos_memory_destroy(&memory);
	return 1;
}
