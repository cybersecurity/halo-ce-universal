#include "guest_address.h"
#include "guest_allocator.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>

struct test_allocator_state
{
	uint32_t next;
	unsigned int releases;
};

static int test_allocate(void *opaque, size_t size, size_t alignment,
	uint32_t *guest_va_out)
{
	struct test_allocator_state *state = opaque;
	uint64_t aligned = ((uint64_t)state->next + alignment - 1) & ~(uint64_t)(alignment - 1);
	if (aligned + size > UINT32_C(0x40004000))
		return -1;
	*guest_va_out = (uint32_t)aligned;
	state->next = (uint32_t)(aligned + size);
	return 0;
}

static void test_release(void *opaque, uint32_t guest_va, size_t size)
{
	struct test_allocator_state *state = opaque;
	assert(guest_va >= UINT32_C(0x40000000));
	assert(size > 0);
	state->releases++;
}

int main(void)
{
	const uint32_t base = UINT32_C(0x40000000);
	const size_t size = 0x4000;
	void *memory = mmap(NULL, size, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	struct test_allocator_state state = { base, 0 };
	struct mac_guest_allocator allocator = { &state, test_allocate, test_release };
	uint32_t guest_va = 0;

	assert(memory != MAP_FAILED);
	assert(mac_guest_address_register(base, memory, size) == 0);
	assert(mac_guest_allocator_install(&allocator) == 0);
	assert(mac_guest_allocate(96, 64, &guest_va) == 0);
	assert(guest_va == base);
	assert(mac_guest_address_resolve(guest_va, 96) == memory);
	assert(mac_guest_release(guest_va, 96) == 0);
	assert(state.releases == 1);
	assert(mac_guest_allocate(32, 3, &guest_va) == -1);
	assert(mac_guest_allocate(size, 16, &guest_va) == -1);
	assert(state.releases == 1);

	mac_guest_address_reset();
	puts("guest_allocator_test: ok");
	return 0;
}
