#include "../guest_address.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

int main(void)
{
	unsigned char backing[32] = { 0 };
	uint32_t guest_address = 0;
	unsigned char *reserved;
	size_t host_page_size = mac_guest_address_host_page_size();
	ptrdiff_t guest_offset;

	mac_guest_address_reset();
	assert(mac_guest_address_register(0x80300000u, backing, sizeof(backing)) == 0);
	assert(mac_guest_address_resolve(0x80300004u, 4) == backing + 4);
	assert(mac_guest_address_resolve(0x8030001fu, 1) == backing + 31);
	assert(mac_guest_address_resolve(0x8030001fu, 2) == NULL);
	assert(mac_guest_address_resolve(0x802fffffu, 1) == NULL);
	assert(mac_guest_address_resolve(0, 1) == NULL);
	assert(mac_guest_address_from_host(backing + 7, 8, &guest_address) == 0);
	assert(guest_address == 0x80300007u);
	assert(mac_guest_address_from_host(backing + 31, 2, &guest_address) == -1);
	assert(mac_guest_address_register(0x80300010u, backing + 16, 8) == -1);
	assert(mac_guest_address_register(0x80400000u, backing + 1, 4) == 0);
	assert(mac_guest_address_resolve(0x80400001u, 2) == backing + 2);
	assert(mac_guest_address_from_host(backing + 2, 1, &guest_address) == -1);
	assert(mac_guest_address_register(0xfffffff0u, backing, 32) == -1);
	assert(mac_guest_address_unregister(0x80400000u) == 0);
	assert(mac_guest_address_unregister(0x80300000u) == 0);
	assert(mac_guest_address_resolve(0x80300004u, 4) == NULL);
	assert(mac_guest_address_unregister(0x80300000u) == -1);
	mac_guest_address_reset();

	assert(host_page_size >= 0x1000u && (host_page_size & (host_page_size - 1)) == 0);
	reserved = mac_guest_address_reserve(0x70000000u, host_page_size * 2);
	assert(reserved != NULL);
	assert(mac_guest_address_resolve(0x70000000u, 1) == reserved);
	assert(mac_guest_address_commit(0x70000000u, host_page_size,
		PROT_READ | PROT_WRITE) == 0);
	assert(mac_guest_address_resolve(0x70000000u, 1) == reserved);
	reserved[0] = 0x5A;
	assert(mac_guest_address_protect(0x70000000u, host_page_size, PROT_READ) == 0);
	assert(mac_guest_address_resolve(0x70000000u, 1) == reserved);
	assert(mac_guest_address_commit(0x70000000u + 0x1000u, 0x1000u,
		PROT_READ | PROT_WRITE) == 0);
	assert(mac_guest_address_unregister(0x70000000u) == 0);
	assert(mac_guest_address_resolve(0x70000000u, 1) == NULL);

	reserved = mac_guest_address_reserve(0, (size_t)UINT64_C(0x100000000));
	assert(reserved != NULL);
	assert(mac_guest_address_resolve(0, 1) == NULL);
	assert(mac_guest_address_resolve(0x80000000u, 1) == reserved + 0x80000000u);
	assert(mac_guest_address_from_host(reserved + 0x80000000u, 1, &guest_address) == 0);
	assert(guest_address == 0x80000000u);
	assert(mac_guest_address_linear_offset(0, reserved, &guest_offset) == 0);
	assert((uintptr_t)(guest_offset + (ptrdiff_t)0x80000000u) ==
		(uintptr_t)(reserved + 0x80000000u));
	assert(mac_guest_address_unregister(0) == 0);
	return 0;
}
