#include "guest_address.h"
#include "guest_allocator.h"
#include "guest_heap.h"
#include "host_syscall.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

#define SYS_MMAP 222
#define SYS_MUNMAP 215
#define SYS_MPROTECT 226
#define LINUX_MAP_PRIVATE 0x02
#define LINUX_MAP_FIXED 0x10
#define LINUX_MAP_ANONYMOUS 0x20
#define LINUX_MAP_NORESERVE 0x4000
#define LINUX_MAP_FIXED_NOREPLACE 0x100000

static int64_t call6(uint64_t number, uint64_t a0, uint64_t a1,
	uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
	uint64_t args[6] = { a0, a1, a2, a3, a4, a5 };
	return mac_host_arm64_32_syscall(number, args);
}

int main(void)
{
	void *arena = mac_guest_address_reserve(0, UINT64_C(1) << 32);
	uint32_t first, again;
	const uint32_t xbox_window = UINT32_C(0x80001000);
	int64_t result;
	assert(arena != NULL);
	assert(mac_guest_heap_install(0x00100000) == 0);
	assert(mac_guest_allocate(9000, 65536, &first) == 0);
	assert((first & 0xffff) == 0);
	uint8_t *guest = mac_guest_address_resolve(first, 9000);
	assert(guest != NULL);
	for (unsigned i = 0; i < 9000; ++i) assert(guest[i] == 0);
	memset(guest, 0xa5, 9000);
	assert(mac_guest_release(first, 9000) == 0);
	assert(mac_guest_allocate(9000, 65536, &again) == 0 && again == first);
	guest = mac_guest_address_resolve(again, 9000);
	for (unsigned i = 0; i < 9000; ++i) assert(guest[i] == 0);

	/* mmap zero-fills before applying the requested final protection. */
	result = call6(SYS_MMAP, 0, 4096, PROT_READ,
		LINUX_MAP_PRIVATE | LINUX_MAP_ANONYMOUS,
		(uint64_t)(int64_t)-1, 0);
	assert(result > 0);
	guest = mac_guest_address_resolve((uint32_t)result, 4096);
	assert(guest != NULL);
	for (unsigned i = 0; i < 4096; ++i) assert(guest[i] == 0);
	assert(call6(SYS_MUNMAP, (uint32_t)result, 4096, 0, 0, 0, 0) == 0);
	/* Reuse formerly poisoned anonymous backing with a short request. The
	 * malloc end guard at the rounded guest-page boundary must be zero. */
	result = call6(SYS_MMAP, 0, 4096, PROT_READ | PROT_WRITE,
		LINUX_MAP_PRIVATE | LINUX_MAP_ANONYMOUS,
		(uint64_t)(int64_t)-1, 0);
	assert(result > 0);
	uint32_t poisoned_va = (uint32_t)result;
	guest = mac_guest_address_resolve(poisoned_va, 4096);
	memset(guest, 0xca, 4096);
	assert(call6(SYS_MUNMAP, poisoned_va, 4096, 0, 0, 0, 0) == 0);
	result = call6(SYS_MMAP, 0, 17, PROT_READ | PROT_WRITE,
		LINUX_MAP_PRIVATE | LINUX_MAP_ANONYMOUS,
		(uint64_t)(int64_t)-1, 0);
	assert(result == poisoned_va);
	guest = mac_guest_address_resolve(poisoned_va, 4096);
	for (unsigned i = 0; i < 4096; ++i) assert(guest[i] == 0);
	assert(guest[4092] == 0);
	assert(call6(SYS_MUNMAP, poisoned_va, 17, 0, 0, 0, 0) == 0);
	result = call6(SYS_MMAP, 0, 4096, PROT_NONE,
		LINUX_MAP_PRIVATE | LINUX_MAP_ANONYMOUS,
		(uint64_t)(int64_t)-1, 0);
	assert(result > 0);
	assert(call6(SYS_MPROTECT, (uint32_t)result, 4096,
		PROT_READ | PROT_WRITE, 0, 0, 0) == 0);
	guest = mac_guest_address_resolve((uint32_t)result, 4096);
	assert(guest != NULL);
	for (unsigned i = 0; i < 4096; ++i) assert(guest[i] == 0);
	assert(call6(SYS_MUNMAP, (uint32_t)result, 4096, 0, 0, 0, 0) == 0);

	/* Linux mmap accepts a 4K subpage address/length while Darwin commits its
	 * enclosing 16K host page. */
	result = call6(SYS_MMAP, xbox_window, 4096, PROT_READ | PROT_WRITE,
		LINUX_MAP_PRIVATE | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS,
		(uint64_t)(int64_t)-1, 0);
	assert(result == xbox_window);
	guest = mac_guest_address_resolve(xbox_window, 4096);
	assert(guest != NULL);
	memset(guest, 0x5a, 4096);
	assert(call6(SYS_MPROTECT, xbox_window, 4096, PROT_READ, 0, 0, 0) == 0);
	assert(call6(SYS_MPROTECT, xbox_window, 4096, PROT_READ | PROT_WRITE, 0, 0, 0) == 0);
	assert(call6(SYS_MUNMAP, xbox_window, 4096, 0, 0, 0, 0) == 0);

	/* Fixed mappings cannot replace the loaded image or the import-stub window. */
	result = call6(SYS_MMAP, 0x10000, 4096, PROT_READ | PROT_WRITE,
		LINUX_MAP_PRIVATE | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS,
		(uint64_t)(int64_t)-1, 0);
	assert(result == -12); /* ENOMEM: collision with reserved image pages */
	result = call6(SYS_MMAP, 0x70000000, 4096, PROT_READ | PROT_WRITE,
		LINUX_MAP_PRIVATE | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS,
		(uint64_t)(int64_t)-1, 0);
	assert(result == -12);
	result = call6(SYS_MMAP, 0x80010000, 4096, PROT_READ | PROT_EXEC,
		LINUX_MAP_PRIVATE | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS,
		(uint64_t)(int64_t)-1, 0);
	assert(result == -13); /* EACCES: native arena pages are never executable */

	/* The Linux Xbox layer reserves the 128 MiB guest physical window with
	 * MAP_FIXED_NOREPLACE|MAP_NORESERVE, then overlays allocated blocks using
	 * MAP_FIXED. Both operations stay inside the already-reserved high arena. */
	result = call6(SYS_MMAP, 0x80000000, 0x08000000, PROT_NONE,
		LINUX_MAP_PRIVATE | LINUX_MAP_FIXED_NOREPLACE | LINUX_MAP_ANONYMOUS |
		LINUX_MAP_NORESERVE, (uint64_t)(int64_t)-1, 0);
	assert(result == 0x80000000);
	result = call6(SYS_MMAP, 0x80000000, 0x08000000, PROT_NONE,
		LINUX_MAP_PRIVATE | LINUX_MAP_FIXED_NOREPLACE | LINUX_MAP_ANONYMOUS |
		LINUX_MAP_NORESERVE, (uint64_t)(int64_t)-1, 0);
	assert(result == -12); /* a second no-replace reservation collides */

	/* Two independent Xbox 4KB blocks can share one 16KB Darwin host page.
	 * Freeing/recommitting either block must not erase or hide its neighbor. */
	{
		const uint32_t left = 0x80001000;
		const uint32_t right = 0x80002000;
		uint8_t *left_bytes, *right_bytes;
		assert(mac_guest_address_host_page_size() == 16384);
		assert(call6(SYS_MMAP, left, 4096, PROT_READ | PROT_WRITE,
			LINUX_MAP_PRIVATE | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS |
			LINUX_MAP_NORESERVE, (uint64_t)(int64_t)-1, 0) == left);
		left_bytes = mac_guest_address_resolve(left, 4096);
		assert(left_bytes != NULL);
		memset(left_bytes, 0x4c, 4096);
		assert(call6(SYS_MMAP, right, 4096, PROT_READ | PROT_WRITE,
			LINUX_MAP_PRIVATE | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS |
			LINUX_MAP_NORESERVE, (uint64_t)(int64_t)-1, 0) == right);
		right_bytes = mac_guest_address_resolve(right, 4096);
		assert(right_bytes != NULL);
		memset(right_bytes, 0xb2, 4096);
		/* Linux rounds anonymous mmap length up to a guest page. Its tail
		 * must be zero too, even when Darwin retains the former backing. */
		assert(call6(SYS_MMAP, left, 17, PROT_READ | PROT_WRITE,
			LINUX_MAP_PRIVATE | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS,
			(uint64_t)(int64_t)-1, 0) == left);
		for (unsigned i = 0; i < 4096; ++i)
		{
			assert(left_bytes[i] == 0);
			assert(right_bytes[i] == 0xb2);
		}
		assert(call6(SYS_MMAP, left, 4096, PROT_NONE,
			LINUX_MAP_PRIVATE | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS |
			LINUX_MAP_NORESERVE, (uint64_t)(int64_t)-1, 0) == left);
		for (unsigned i = 0; i < 4096; ++i) assert(right_bytes[i] == 0xb2);
		assert(call6(SYS_MMAP, left, 4096, PROT_READ | PROT_WRITE,
			LINUX_MAP_PRIVATE | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS |
			LINUX_MAP_NORESERVE, (uint64_t)(int64_t)-1, 0) == left);
		left_bytes = mac_guest_address_resolve(left, 4096);
		for (unsigned i = 0; i < 4096; ++i)
		{
			assert(left_bytes[i] == 0);
			assert(right_bytes[i] == 0xb2);
		}
		assert(call6(SYS_MMAP, left, 4096, PROT_NONE,
			LINUX_MAP_PRIVATE | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS |
			LINUX_MAP_NORESERVE, (uint64_t)(int64_t)-1, 0) == left);
		for (unsigned i = 0; i < 4096; ++i) assert(right_bytes[i] == 0xb2);
		assert(call6(SYS_MMAP, right, 4096, PROT_NONE,
			LINUX_MAP_PRIVATE | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS |
			LINUX_MAP_NORESERVE, (uint64_t)(int64_t)-1, 0) == right);
	}

	result = call6(SYS_MMAP, 0x81A00000, 16u * 1024u * 1024u,
		PROT_READ | PROT_WRITE,
		LINUX_MAP_PRIVATE | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS |
		LINUX_MAP_NORESERVE, (uint64_t)(int64_t)-1, 0);
	assert(result == 0x81A00000); /* XPhysicalAlloc fixed physical address */
	guest = mac_guest_address_resolve((uint32_t)result, 16u * 1024u * 1024u);
	assert(guest != NULL);
	memset(guest, 0x39, 16u * 1024u * 1024u);
	result = call6(SYS_MMAP, 0x81A00000, 16u * 1024u * 1024u, PROT_NONE,
		LINUX_MAP_PRIVATE | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS |
		LINUX_MAP_NORESERVE, (uint64_t)(int64_t)-1, 0);
	assert(result == 0x81A00000); /* XPhysicalFree restores the reservation */
	assert(call6(SYS_MUNMAP, 0x81A00000, 16u * 1024u * 1024u, 0, 0, 0, 0) == -22);
	assert(call6(SYS_MPROTECT, 0x81A00000, 16u * 1024u * 1024u,
		PROT_READ | PROT_WRITE, 0, 0, 0) == 0);
	guest = mac_guest_address_resolve(0x81A00000, 16u * 1024u * 1024u);
	assert(guest && guest[0] == 0 && guest[16u * 1024u * 1024u - 1] == 0);
	assert(call6(SYS_MMAP, 0x81A00000, 16u * 1024u * 1024u, PROT_NONE,
		LINUX_MAP_PRIVATE | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS |
		LINUX_MAP_NORESERVE, (uint64_t)(int64_t)-1, 0) == 0x81A00000);
	result = call6(SYS_MMAP, 0x81A00000, 16u * 1024u * 1024u,
		PROT_READ | PROT_WRITE,
		LINUX_MAP_PRIVATE | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS |
		LINUX_MAP_NORESERVE, (uint64_t)(int64_t)-1, 0);
	assert(result == 0x81A00000);
	guest = mac_guest_address_resolve((uint32_t)result, 16u * 1024u * 1024u);
	assert(guest && guest[0] == 0 && guest[16u * 1024u * 1024u - 1] == 0);
	assert(call6(SYS_MMAP, 0x81A00000, 16u * 1024u * 1024u, PROT_NONE,
		LINUX_MAP_PRIVATE | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS |
		LINUX_MAP_NORESERVE, (uint64_t)(int64_t)-1, 0) == 0x81A00000);

	/* mmap must clear anonymous bytes before applying read-only or no-access
	 * protection. Temporarily make each mapping writable to inspect the zero
	 * fill, then unmap it. This catches a host-side memset after final mprotect. */
	for (int protection_index = 0; protection_index < 2; ++protection_index)
	{
		int initial_protection = protection_index == 0 ? PROT_READ : PROT_NONE;
		uint32_t mapped_va = 0;
		result = call6(SYS_MMAP, 0, 8192, initial_protection,
			LINUX_MAP_PRIVATE | LINUX_MAP_ANONYMOUS,
			(uint64_t)(int64_t)-1, 0);
		assert(result > 0 && (uint64_t)result <= UINT32_MAX);
		mapped_va = (uint32_t)result;
		assert(call6(SYS_MPROTECT, mapped_va, 8192, PROT_READ | PROT_WRITE,
			0, 0, 0) == 0);
		guest = mac_guest_address_resolve(mapped_va, 8192);
		assert(guest != NULL);
		for (unsigned i = 0; i < 8192; ++i) assert(guest[i] == 0);
		assert(call6(SYS_MUNMAP, mapped_va, 8192, 0, 0, 0, 0) == 0);
	}

	assert(mac_guest_release(again, 9000) == 0);
	assert(mac_guest_address_unregister(0) == 0);
	return 0;
}
