/*
HOST_WATCH_HASH_TEST.C

Tests of the page-hash write tracking (port/android/host/host_watch_hash.c).
Built and run by tools/test_host_watch_hash.py; exits nonzero on a failure.
*/

#include "../host/host_watch_hash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGES 4

static int failures;

#define CHECK(condition) \
	do \
	{ \
		if (!(condition)) \
		{ \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
			failures++; \
		} \
	} while (0)

struct fixture
{
	uint8_t *memory;
	uint8_t watched[PAGES];
	uint64_t hash[PAGES];
	uint32_t hashed_frame[PAGES];
	uint32_t generation[PAGES];
	volatile uint32_t current;
	struct watch_hash watch;
};

static void setup(struct fixture *f)
{
	memset(f, 0, sizeof(*f));
	f->memory = aligned_alloc(WATCH_HASH_PAGE_SIZE, PAGES * WATCH_HASH_PAGE_SIZE);
	memset(f->memory, 0x5a, PAGES * WATCH_HASH_PAGE_SIZE);
	f->current = 1;
	f->watch.base = f->memory;
	f->watch.page_count = PAGES;
	f->watch.watched = f->watched;
	f->watch.hash = f->hash;
	f->watch.hashed_frame = f->hashed_frame;
	f->watch.generation = f->generation;
	f->watch.current_generation = &f->current;
	f->watch.frame = 1;
}

static void teardown(struct fixture *f)
{
	free(f->memory);
}

static uint8_t *byte_of(struct fixture *f, uint32_t page, uint32_t offset)
{
	return f->memory + page * WATCH_HASH_PAGE_SIZE + offset;
}

static void test_unchanged_page_keeps_generation(void)
{
	struct fixture f;
	uint32_t before;

	setup(&f);
	watch_hash_protect(&f.watch, 0, 0);
	before = watch_hash_generation(&f.watch, 0, 0);
	watch_hash_begin_frame(&f.watch);
	CHECK(watch_hash_generation(&f.watch, 0, 0) == before);
	teardown(&f);
}

static void test_write_is_seen_next_frame(void)
{
	struct fixture f;
	uint32_t before, after;

	setup(&f);
	watch_hash_protect(&f.watch, 1, 1);
	before = watch_hash_generation(&f.watch, 1, 1);
	*byte_of(&f, 1, 100) ^= 1;
	/* the page was hashed in this frame already */
	CHECK(watch_hash_generation(&f.watch, 1, 1) == before);
	watch_hash_begin_frame(&f.watch);
	after = watch_hash_generation(&f.watch, 1, 1);
	CHECK(after > before);
	CHECK(f.current >= after);
	teardown(&f);
}

static void test_any_byte_is_seen(void)
{
	static const uint32_t offsets[] = { 0, 2047, WATCH_HASH_PAGE_SIZE - 1 };
	unsigned index;

	for (index = 0; index < sizeof(offsets) / sizeof(offsets[0]); index++)
	{
		struct fixture f;
		uint32_t before;

		setup(&f);
		watch_hash_protect(&f.watch, 2, 2);
		watch_hash_begin_frame(&f.watch);
		before = watch_hash_generation(&f.watch, 2, 2);
		*byte_of(&f, 2, offsets[index]) += 1;
		watch_hash_begin_frame(&f.watch);
		CHECK(watch_hash_generation(&f.watch, 2, 2) > before);
		teardown(&f);
	}
}

static void test_hash_at_most_once_per_frame(void)
{
	struct fixture f;
	uint32_t before;

	setup(&f);
	watch_hash_protect(&f.watch, 0, 0);
	watch_hash_begin_frame(&f.watch);
	before = watch_hash_generation(&f.watch, 0, 0);
	CHECK(f.hashed_frame[0] == f.watch.frame);
	*byte_of(&f, 0, 8) ^= 0xff;
	CHECK(watch_hash_generation(&f.watch, 0, 0) == before);
	watch_hash_begin_frame(&f.watch);
	CHECK(watch_hash_generation(&f.watch, 0, 0) > before);
	teardown(&f);
}

static void test_unwatched_page_is_not_hashed(void)
{
	struct fixture f;

	setup(&f);
	*byte_of(&f, 3, 0) ^= 1;
	watch_hash_begin_frame(&f.watch);
	CHECK(watch_hash_generation(&f.watch, 3, 3) == 0);
	CHECK(f.hashed_frame[3] == 0);
	teardown(&f);
}

static void test_forget_unwatches_and_bumps(void)
{
	struct fixture f;
	uint32_t before;

	setup(&f);
	watch_hash_protect(&f.watch, 2, 2);
	before = watch_hash_generation(&f.watch, 2, 2);
	watch_hash_forget(&f.watch, 2, 2);
	CHECK(f.watched[2] == 0);
	CHECK(watch_hash_generation(&f.watch, 2, 2) > before);
	teardown(&f);
}

static void test_range_returns_newest(void)
{
	struct fixture f;
	uint32_t newest;

	setup(&f);
	watch_hash_protect(&f.watch, 0, 3);
	watch_hash_begin_frame(&f.watch);
	*byte_of(&f, 3, 5) ^= 1;
	watch_hash_begin_frame(&f.watch);
	newest = watch_hash_generation(&f.watch, 0, 3);
	CHECK(newest == f.generation[3]);
	CHECK(watch_hash_generation(&f.watch, 0, 2) < newest);
	teardown(&f);
}

static void test_begin_frame_changes_serial(void)
{
	struct fixture f;
	uint32_t serial;

	setup(&f);
	serial = f.current;
	watch_hash_begin_frame(&f.watch);
	CHECK(f.current != serial);
	teardown(&f);
}

static void test_ranges_are_clamped(void)
{
	struct fixture f;

	setup(&f);
	watch_hash_protect(&f.watch, 2, 99);
	CHECK(f.watched[2] == 1 && f.watched[3] == 1);
	CHECK(watch_hash_generation(&f.watch, 5, 9) == 0);
	watch_hash_forget(&f.watch, 7, 8);
	teardown(&f);
}

int main(void)
{
	test_unchanged_page_keeps_generation();
	test_write_is_seen_next_frame();
	test_any_byte_is_seen();
	test_hash_at_most_once_per_frame();
	test_unwatched_page_is_not_hashed();
	test_forget_unwatches_and_bumps();
	test_range_returns_newest();
	test_begin_frame_changes_serial();
	test_ranges_are_clamped();
	if (failures)
		printf("%d failures\n", failures);
	else
		printf("all tests passed\n");
	return failures != 0;
}
