#include "guest_import_registry.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static int first_calls;
static int second_calls;

static void first_bridge(void)
{
	first_calls++;
}

static void second_bridge(void)
{
	second_calls++;
}

int main(void)
{
	uint32_t first_token = 0, second_token = 0;
	mac_guest_function first;
	mac_guest_function second;

	assert(mac_guest_import_register("host_syscall", first_bridge, &first_token) == 0);
	assert(mac_guest_import_register("host_thread_create", second_bridge, &second_token) == 0);
	assert(first_token == MAC_GUEST_IMPORT_TOKEN_BASE);
	assert(second_token == MAC_GUEST_IMPORT_TOKEN_BASE + MAC_GUEST_IMPORT_TOKEN_STRIDE);
	assert(mac_guest_import_count() == 2);
	assert(mac_guest_import_token("host_syscall") == first_token);
	assert(mac_guest_import_register("host_syscall", first_bridge, NULL) == 0);
	assert(mac_guest_import_register("host_syscall", second_bridge, NULL) == -1);

	first = mac_guest_import_resolve(first_token);
	second = mac_guest_import_resolve(second_token);
	assert(first && second && first != second);
	first();
	second();
	assert(first_calls == 1 && second_calls == 1);
	assert(mac_guest_import_resolve(first_token + 1) == NULL);
	assert(mac_guest_import_resolve(MAC_GUEST_IMPORT_TOKEN_BASE + 0x1000) == NULL);

	mac_guest_import_reset();
	assert(mac_guest_import_count() == 0);
	assert(mac_guest_import_resolve(first_token) == NULL);
	puts("guest_import_registry_test: ok");
	return 0;
}
