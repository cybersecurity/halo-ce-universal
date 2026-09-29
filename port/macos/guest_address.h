/*
 * Guest virtual-address mapping helpers for the native macOS port.
 *
 * Xbox cache structures and guest ABIs contain 32-bit virtual addresses.
 * Native arm64 host pointers are 64-bit and must never be truncated to fit
 * those fields.  Register each mapped guest range against its host backing
 * and resolve guest addresses explicitly at the host boundary.
 */

#ifndef HALO_MACOS_GUEST_ADDRESS_H
#define HALO_MACOS_GUEST_ADDRESS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Register one non-overlapping guest VA range and its host backing. Multiple
 * guest ranges may alias the same host bytes. The entire range must fit in
 * the 32-bit Xbox address space. Returns 0 on success, -1 for invalid,
 * guest-overlapping, or full-table registrations. */
int mac_guest_address_register(uint32_t guest_base, void *host_base, size_t size);

/* Reserve a guest VA range in a high, OS-chosen host address range. The
 * reservation is PROT_NONE and does not commit backing pages. This allows
 * translated code to keep Xbox VAs while host code resolves through the
 * registered offset map. Returns the host base or NULL. */
void *mac_guest_address_reserve(uint32_t guest_base, size_t size);

/* Commit anonymous pages in an owned reservation by changing their
 * protection. File-backed mappings need a separate Darwin VM path. */
int mac_guest_address_commit(uint32_t guest_address, size_t size, int protection);
int mac_guest_address_protect(uint32_t guest_address, size_t size, int protection);
size_t mac_guest_address_host_page_size(void);

/* Remove a range previously registered at guest_base. Returns 0 on success. */
int mac_guest_address_unregister(uint32_t guest_base);

/* Resolve a guest VA span. The full span must lie in one registered range;
 * overflow, unmapped spans and null guest VA return NULL. */
void *mac_guest_address_resolve(uint32_t guest_address, size_t size);
/* Number of contiguous bytes available from guest_address to the end of its
 * registered range, or zero if the address is not mapped/null. */
size_t mac_guest_address_available(uint32_t guest_address);

/* Convert a host span back to a guest VA. The full span must be contained in
 * exactly one registered host backing; aliases are ambiguous and fail.
 * Returns 0 on success and -1 otherwise. */
int mac_guest_address_from_host(const void *host_address, size_t size,
	uint32_t *guest_address_out);

/* Offset suitable for the translated runtime's XBOX_PTR/ MEM macros. This is
 * the host-base minus guest-base for a registered range and is valid only
 * when all guest memory shares that one linear backing arena. */
int mac_guest_address_linear_offset(uint32_t guest_base, const void *host_base,
	ptrdiff_t *offset_out);

/* Clear all registrations. Call only after guest threads have stopped. */
void mac_guest_address_reset(void);

#ifdef __cplusplus
}
#endif

#endif
