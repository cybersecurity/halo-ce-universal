#include "host_memory_fingerprint.h"
#include "guest_address.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* Original guest algorithm, retained independently for bit-exact checks. */
static void original(const volatile unsigned char *bytes, uint64_t output[2])
{
	uint64_t a = UINT64_C(14695981039346656037), b = UINT64_C(0x9e3779b97f4a7c15);
	for (unsigned i = 0; i < 4096; ++i)
	{
		uint64_t value = bytes[i];
		a = (a ^ value) * UINT64_C(1099511628211);
		b ^= value + UINT64_C(0x9e3779b97f4a7c15) + (b << 6) + (b >> 2);
		b *= UINT64_C(0xbf58476d1ce4e5b9);
	}
	output[0] = a; output[1] = b;
}

int main(void)
{
	unsigned char memory[8192] = {0};
	uint64_t expected[2], actual[2], before[2];
	const uint32_t base = 0x30000000;
	assert(mac_guest_address_register(base, memory, sizeof(memory)) == 0);
	for (unsigned pass = 0; pass < 4; ++pass)
	{
		unsigned offset = pass == 1 ? 1 : 0;
		if (pass == 1)
		{
			uint32_t state = 0x12345678;
			for (unsigned i = 0; i < 4097; ++i)
			{ state = state * 1664525u + 1013904223u; memory[i] = (unsigned char)(state >> 24); }
		}
		if (pass == 2) memory[0] ^= 0x80;
		if (pass == 3) memory[4095] ^= 0x01;
		original(memory + offset, expected);
		assert(mac_host_memory_fingerprint_page(base + offset, base + 5001));
		memcpy(actual, memory + 5001, sizeof(actual));
		assert(!memcmp(actual, expected, sizeof(actual)));
		if (pass == 3) assert(memcmp(actual, before, sizeof(actual)) != 0);
		memcpy(before, actual, sizeof(before));
	}
	/* Every byte may change without prepare_write. All such writes must be
	 * detected, and unchanged scans must remain exactly bit-identical. */
	struct halo_fingerprint_stats stats_before, stats_after;
	mac_host_memory_fingerprint_stats(&stats_before);
	for (unsigned i = 0; i < 32; ++i) assert(mac_host_memory_fingerprint_page(base, base + 5001));
	mac_host_memory_fingerprint_stats(&stats_after);
	assert(stats_after.identical - stats_before.identical == 32);
	for (unsigned i = 0; i < 4096; ++i)
	{
		memcpy(before, memory + 5001, sizeof(before));
		memory[i] ^= 1;
		original(memory, expected);
		assert(mac_host_memory_fingerprint_page(base, base + 5001));
		memcpy(actual, memory + 5001, sizeof(actual));
		assert(!memcmp(actual, expected, sizeof(actual)));
		assert(memcmp(actual, before, sizeof(actual)));
	}
	/* Four colliding pages should coexist. A fifth evicts the oldest, whose
	 * next query must rehash rather than incorrectly reuse another page. */
	const unsigned collision_stride = 8 * 1024 * 1024;
	const size_t collision_size = 4 * collision_stride + 4096;
	unsigned char *collision = calloc(1, collision_size);
	const uint32_t collision_base = 0x40001000;
	assert(collision && !mac_guest_address_register(collision_base, collision, collision_size));
	for (unsigned i = 0; i < 5; ++i) collision[i * collision_stride] = (unsigned char)(i + 1);
	mac_host_memory_fingerprint_stats(&stats_before);
	for (unsigned i = 0; i < 4; ++i)
		assert(mac_host_memory_fingerprint_page(collision_base + i * collision_stride, base + 5001));
	for (unsigned i = 0; i < 4; ++i)
		assert(mac_host_memory_fingerprint_page(collision_base + i * collision_stride, base + 5001));
	mac_host_memory_fingerprint_stats(&stats_after);
	assert(stats_after.identical - stats_before.identical == 4);
	assert(stats_after.collisions == stats_before.collisions);
	for (unsigned i = 0; i < 10; ++i)
	{
		unsigned offset = (i % 5) * collision_stride;
		original(collision + offset, expected);
		assert(mac_host_memory_fingerprint_page(collision_base + offset, base + 5001));
		memcpy(actual, memory + 5001, sizeof(actual));
		assert(!memcmp(actual, expected, sizeof(actual)));
	}
	mac_host_memory_fingerprint_stats(&stats_after);
	assert(stats_after.collisions - stats_before.collisions >= 6);
	/* Compare batched range generations with the original independent page
	 * hashes for overlapping subranges, dirty and explicitly invalidated pages. */
	unsigned char *range = calloc(1, 32768);
	const uint32_t range_base = 0x50000000;
	assert(range && !mac_guest_address_register(range_base, range, 32768));
	struct wire_entry { uint64_t hashes[2]; uint32_t generation; unsigned char valid, padding[3]; };
	_Static_assert(sizeof(struct wire_entry) == 24, "guest fingerprint wire");
	struct wire_entry reference[4] = {{0}};
	uint32_t reference_counter = 1, counter = 1;
	memcpy(range + 24000, &counter, 4);
	for (unsigned pass = 0; pass < 8; ++pass)
	{
		unsigned begin = pass % 3, pages = 4 - begin;
		if (pass == 1) range[4096 + 9] ^= 0x80;
		if (pass == 3) range[3 * 4096 + 4095] ^= 0x01;
		if (pass == 5)
		{
			reference[2].valid = 0;
			range[20000 + 2 * 24 + 20] = 0;
			++reference_counter;
			memcpy(range + 24000, &reference_counter, 4);
		}
		uint32_t newest = 0, native_newest;
		for (unsigned page = begin; page < begin + pages; ++page)
		{
			original(range + page * 4096, expected);
			if (!reference[page].valid || memcmp(reference[page].hashes, expected, 16))
			{
				memcpy(reference[page].hashes, expected, 16);
				reference[page].valid = 1;
				reference[page].generation = ++reference_counter;
			}
			if (reference[page].generation > newest) newest = reference[page].generation;
		}
		assert(mac_host_memory_fingerprint_range(range_base + begin * 4096, pages,
			range_base + 20000 + begin * 24, range_base + 24000, range_base + 24004));
		memcpy(&native_newest, range + 24004, 4);
		memcpy(&counter, range + 24000, 4);
		assert(native_newest == newest && counter == reference_counter);
		assert(!memcmp(range + 20000, reference, sizeof(reference)));
	}
	unsigned char unchanged[104]; memcpy(unchanged, range + 20000, 96); memcpy(unchanged + 96, range + 24000, 8);
	assert(!mac_host_memory_fingerprint_range(range_base + 30000, 2, range_base + 20000, range_base + 24000, range_base + 24004));
	assert(!mac_host_memory_fingerprint_range(range_base, 2, range_base + 32760, range_base + 24000, range_base + 24004));
	assert(!mac_host_memory_fingerprint_range(range_base, 2, range_base + 20000, range_base + 24001, range_base + 24004));
	assert(!mac_host_memory_fingerprint_range(range_base, 0, range_base + 20000, range_base + 24000, range_base + 24004));
	assert(!memcmp(unchanged, range + 20000, 96) && !memcmp(unchanged + 96, range + 24000, 8));
	/* A focused repeated-static-page benchmark compares exactly the same
	 * checked hash output with the former dependent-multiply byte scan. */
	const unsigned iterations = 10000;
	struct timespec start, end;
	clock_gettime(CLOCK_MONOTONIC, &start);
	for (unsigned i = 0; i < iterations; ++i) original(memory, expected);
	clock_gettime(CLOCK_MONOTONIC, &end);
	double original_ms = (end.tv_sec-start.tv_sec)*1000.0 + (end.tv_nsec-start.tv_nsec)/1e6;
	clock_gettime(CLOCK_MONOTONIC, &start);
	for (unsigned i = 0; i < iterations; ++i) assert(mac_host_memory_fingerprint_page(base, base + 5001));
	clock_gettime(CLOCK_MONOTONIC, &end);
	double cached_ms = (end.tv_sec-start.tv_sec)*1000.0 + (end.tv_nsec-start.tv_nsec)/1e6;
	memcpy(actual, memory + 5001, sizeof(actual));
	assert(!memcmp(actual, expected, sizeof(actual)));
	printf("10000 full-page validations: original %.3f ms snapshot %.3f ms\n",original_ms,cached_ms);
	memset(memory + 5001, 0xa5, 16);
	assert(!mac_host_memory_fingerprint_page(base + 4097, base + 5001));
	for (unsigned i = 5001; i < 5017; ++i) assert(memory[i] == 0xa5);
	assert(!mac_host_memory_fingerprint_page(base, base + 8180));
	assert(!mac_host_memory_fingerprint_page(0, base + 5001));
	assert(!mac_host_memory_fingerprint_page(base, 0));
	assert(!mac_host_memory_fingerprint_page(0xfffff001, base + 5001));
	mac_guest_address_reset(); free(collision); free(range);
	puts("host_memory_fingerprint_test: exact dual hashes, all-byte mutations, repeated validation, collisions, alignment and bounds passed");
	return 0;
}
