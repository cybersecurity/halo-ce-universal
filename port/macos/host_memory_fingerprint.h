#ifndef HALO_HOST_MEMORY_FINGERPRINT_H
#define HALO_HOST_MEMORY_FINGERPRINT_H
#include <stdint.h>
/* Read exactly one guest 4 KiB page and write both unchanged source-port
 * fingerprints to one checked 16-byte guest span. Reuse is permitted only
 * after comparing every current byte against a retained snapshot. */
int mac_host_memory_fingerprint_page(uint32_t address_va, uint32_t hashes_va);
int mac_host_memory_fingerprint_range(uint32_t address_va, uint32_t pages,
	uint32_t entries_va, uint32_t counter_va, uint32_t newest_va);
struct halo_fingerprint_stats { uint64_t scans, identical, rehashed, collisions, scan_ns; };
void mac_host_memory_fingerprint_stats(struct halo_fingerprint_stats *output);
void mac_host_memory_fingerprint_presented(void);
#endif
