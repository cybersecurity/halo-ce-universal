#include "host_memory_fingerprint.h"
#include "guest_address.h"
#include "guest_call.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Four-way mapping bounds storage to 32 MiB. Collisions only evict a snapshot;
 * every successful call still reads/validates all 4096 current guest bytes. */
#define SNAPSHOT_SETS 2048
#define SNAPSHOT_WAYS 4
struct page_snapshot { unsigned char bytes[4096]; uint64_t hashes[2]; uint64_t touched; uint32_t address; int valid; };
static struct page_snapshot snapshots[SNAPSHOT_SETS][SNAPSHOT_WAYS];
static uint64_t snapshot_clock;
static pthread_mutex_t snapshot_lock = PTHREAD_MUTEX_INITIALIZER;
static struct halo_fingerprint_stats stats;
static int metrics_enabled = -1;
static uint64_t report_ns, report_frames;
static struct halo_fingerprint_stats reported;

static uint64_t monotonic_ns(void)
{
	struct timespec value;
	clock_gettime(CLOCK_MONOTONIC, &value);
	return (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec;
}

/* Caller owns snapshot_lock; range calls avoid repeating the ABI boundary,
 * address lookup and lock acquisition for every page of a resource. */
static const uint64_t *fingerprint_locked(uint32_t address_va, const unsigned char *bytes)
{
	if (metrics_enabled < 0) metrics_enabled = getenv("HALO_FINGERPRINT_METRICS") != NULL;
	uint64_t started = metrics_enabled ? monotonic_ns() : 0;
	struct page_snapshot *set = snapshots[(address_va >> 12) % SNAPSHOT_SETS];
	struct page_snapshot *entry = NULL;
	for (unsigned way = 0; way < SNAPSHOT_WAYS; ++way)
		if (set[way].valid && set[way].address == address_va) { entry = &set[way]; break; }
	if (!entry)
	{
		entry = &set[0];
		for (unsigned way = 0; way < SNAPSHOT_WAYS; ++way)
		{
			if (!set[way].valid) { entry = &set[way]; break; }
			if (set[way].touched < entry->touched) entry = &set[way];
		}
	}
	entry->touched = ++snapshot_clock;
	++stats.scans;
	if (entry->valid && entry->address == address_va && !memcmp(bytes, entry->bytes, sizeof(entry->bytes)))
		++stats.identical;
	else
	{
		/* Hash the same snapshot we retain. Copying AFTER hashing live bytes
		 * could associate an old hash with newer concurrent guest writes. */
		if (entry->valid && entry->address != address_va) ++stats.collisions;
		memcpy(entry->bytes, bytes, sizeof(entry->bytes));
		entry->address = address_va;
		entry->hashes[0] = UINT64_C(14695981039346656037);
		entry->hashes[1] = UINT64_C(0x9e3779b97f4a7c15);
		for (unsigned i = 0; i < 4096; ++i)
		{
			uint64_t value = entry->bytes[i];
			entry->hashes[0] = (entry->hashes[0] ^ value) * UINT64_C(1099511628211);
			entry->hashes[1] ^= value + UINT64_C(0x9e3779b97f4a7c15) + (entry->hashes[1] << 6) + (entry->hashes[1] >> 2);
			entry->hashes[1] *= UINT64_C(0xbf58476d1ce4e5b9);
		}
		entry->valid = 1;
		++stats.rehashed;
	}
	if (metrics_enabled) stats.scan_ns += monotonic_ns() - started;
	return entry->hashes;
}

int mac_host_memory_fingerprint_page(uint32_t address_va, uint32_t hashes_va)
{
	const unsigned char *bytes = mac_guest_address_resolve(address_va, 4096);
	uint64_t hashes[2];
	if (!bytes || !mac_guest_address_resolve(hashes_va, sizeof(hashes))) return 0;
	pthread_mutex_lock(&snapshot_lock);
	memcpy(hashes, fingerprint_locked(address_va, bytes), sizeof(hashes));
	pthread_mutex_unlock(&snapshot_lock);
	return mac_guest_write(hashes_va, hashes, sizeof(hashes)) == 0;
}

int mac_host_memory_fingerprint_range(uint32_t address_va, uint32_t pages,
	uint32_t entries_va, uint32_t counter_va, uint32_t newest_va)
{
	/* Guest page_fingerprint is 24 bytes: hashes at0/8, generation at16,
	 * valid at20. Validate all spans before changing any generation. */
	if (!pages || pages > 65536 || (counter_va & 3)) return 0;
	const unsigned char *bytes = mac_guest_address_resolve(address_va, (size_t)pages * 4096);
	unsigned char *entries = mac_guest_address_resolve(entries_va, (size_t)pages * 24);
	uint32_t *counter = mac_guest_address_resolve(counter_va, sizeof(*counter));
	if (!bytes || !entries || !counter || !mac_guest_address_resolve(newest_va, 4)) return 0;
	uint32_t newest = 0;
	pthread_mutex_lock(&snapshot_lock);
	for (uint32_t page = 0; page < pages; ++page)
	{
		unsigned char *entry = entries + (size_t)page * 24;
		const uint64_t *hashes = fingerprint_locked(address_va + page * 4096, bytes + (size_t)page * 4096);
		uint32_t generation;
		if (!entry[20] || memcmp(entry, hashes, 16))
		{
			memcpy(entry, hashes, 16);
			entry[20] = 1;
			generation = __atomic_add_fetch(counter, 1, __ATOMIC_SEQ_CST);
			memcpy(entry + 16, &generation, sizeof(generation));
		}
		else memcpy(&generation, entry + 16, sizeof(generation));
		if (generation > newest) newest = generation;
	}
	pthread_mutex_unlock(&snapshot_lock);
	return mac_guest_write(newest_va, &newest, sizeof(newest)) == 0;
}

void mac_host_memory_fingerprint_stats(struct halo_fingerprint_stats *output)
{
	pthread_mutex_lock(&snapshot_lock); *output = stats; pthread_mutex_unlock(&snapshot_lock);
}

void mac_host_memory_fingerprint_presented(void)
{
	pthread_mutex_lock(&snapshot_lock);
	if (metrics_enabled > 0)
	{
		uint64_t now = monotonic_ns();
		++report_frames;
		if (!report_ns) report_ns = now;
		if (now - report_ns >= UINT64_C(1000000000))
		{
			uint64_t scans = stats.scans - reported.scans;
			fprintf(stderr,"[fingerprint] frames=%llu scans=%llu scans_per_frame=%.1f read_MiB=%.3f identical=%llu rehashed=%llu collisions=%llu scan_ms=%.3f\n",
				(unsigned long long)report_frames,(unsigned long long)scans,(double)scans/report_frames,
				(double)scans*4096/(1024*1024),(unsigned long long)(stats.identical-reported.identical),
				(unsigned long long)(stats.rehashed-reported.rehashed),(unsigned long long)(stats.collisions-reported.collisions),(double)(stats.scan_ns-reported.scan_ns)/1e6);
			reported = stats; report_frames = 0; report_ns = now;
		}
	}
	pthread_mutex_unlock(&snapshot_lock);
}
