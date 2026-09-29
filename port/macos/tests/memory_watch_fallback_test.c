#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "memory_watch_test_platform.h"

unsigned char halo_watch_test_arena[0x10000];

void memory_watch_initialize(void);
void memory_watch_protect(unsigned long address, unsigned long size);
unsigned long memory_watch_generation(unsigned long address, unsigned long size);
unsigned long memory_watch_serial(void);
void memory_watch_prepare_write(void *address, unsigned long size);
void memory_watch_forget(void *address, unsigned long size);

int platform_is_contiguous(const void *address)
{
	uintptr_t value = (uintptr_t)address;
	uintptr_t base = (uintptr_t)halo_watch_test_arena;
	return value >= base && value - base < sizeof(halo_watch_test_arena);
}

int main(void)
{
	unsigned long address = (unsigned long)halo_watch_test_arena + 0x1ff0;
	unsigned long before, unchanged, changed, prepared, forgotten;

	memset(halo_watch_test_arena, 0, sizeof(halo_watch_test_arena));
	memory_watch_initialize();
	before = memory_watch_generation(address, 32);
	unchanged = memory_watch_generation(address, 32);
	assert(before != 0);
	assert(unchanged == before);

	halo_watch_test_arena[0x2005] = 0x5a;
	changed = memory_watch_generation(address, 32);
	assert(changed > unchanged);
	assert(memory_watch_generation(address, 32) == changed);

	memory_watch_prepare_write((void *)address, 32);
	prepared = memory_watch_generation(address, 32);
	assert(prepared > changed);
	memory_watch_forget((void *)address, 32);
	forgotten = memory_watch_generation(address, 32);
	assert(forgotten > prepared);

	assert(memory_watch_generation(address, PLATFORM_CONTIGUOUS_SIZE) == 0);
	assert(memory_watch_generation(address + PLATFORM_CONTIGUOUS_SIZE, 1) == 0);
	assert(memory_watch_serial() != memory_watch_serial());
	puts("macOS content-fingerprint memory-watch fallback: PASS");
	return 0;
}
