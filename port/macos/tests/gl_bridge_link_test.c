#include "../gl_bridge_support.h"
#include "../gl_host_imports.h"
#include "../gl_native_proc.h"
#include "../gl_token_registry.h"
#include "../guest_address.h"
#include "../guest_allocator.h"
#include "../guest_call.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

_Thread_local uint32_t g_eax;
_Thread_local uint32_t g_esp;

#define TEST_GUEST_BASE 0x10000u
#define TEST_GUEST_SIZE 0x10000u

static uint8_t guest_backing[TEST_GUEST_SIZE];
static uint32_t allocation_cursor = TEST_GUEST_BASE + 0x8000u;
static uint32_t import_esp = TEST_GUEST_BASE + 0x400u;

static int test_allocate(void *context, size_t size, size_t alignment,
	uint32_t *guest_va_out)
{
	(void)context;
	if (!size || !alignment || !guest_va_out)
		return -1;
	uint64_t address = ((uint64_t)allocation_cursor + alignment - 1) &
		~((uint64_t)alignment - 1);
	if (address + size > TEST_GUEST_BASE + TEST_GUEST_SIZE)
		return -1;
	*guest_va_out = (uint32_t)address;
	allocation_cursor = (uint32_t)(address + size);
	return 0;
}

static void test_release(void *context, uint32_t guest_va, size_t size)
{
	(void)context;
	(void)guest_va;
	(void)size;
}

uint32_t mac_guest_host_import_arg32(uint32_t index)
{
	uint32_t value = 0;
	assert(mac_guest_read(import_esp + 4u + index * 4u, &value, sizeof(value)) == 0);
	return value;
}

void mac_guest_host_import_return32(uint32_t value)
{
	g_eax = value;
	import_esp += 4;
}

void mac_guest_host_import_return_void(void)
{
	import_esp += 4;
}

void mac_host_abort(const char *message)
{
	(void)message;
	abort();
}

int main(void)
{
	const struct mac_guest_allocator allocator = {
		NULL, test_allocate, test_release
	};
	const uint32_t known_name_va = TEST_GUEST_BASE + 0x100u;
	const uint32_t unknown_name_va = TEST_GUEST_BASE + 0x120u;
	const uint32_t proc_name_va = TEST_GUEST_BASE + 0x140u;
	const uint32_t get_string_args[] = { 0x1F02u };
	uint32_t version_va;

	assert(mac_guest_address_register(TEST_GUEST_BASE, guest_backing,
		sizeof(guest_backing)) == 0);
	memcpy(guest_backing + (known_name_va - TEST_GUEST_BASE), "glViewport", sizeof("glViewport"));
	memcpy(guest_backing + (unknown_name_va - TEST_GUEST_BASE), "glNoSuchCall", sizeof("glNoSuchCall"));
	memcpy(guest_backing + (proc_name_va - TEST_GUEST_BASE), "glViewport", sizeof("glViewport"));
	assert(mac_guest_allocator_install(&allocator) == 0);
	assert(mac_macos_gl_token_for_guest_name(known_name_va) == 0xFE100080u);
	assert(mac_macos_gl_token_for_guest_name(unknown_name_va) == 0);
	assert(mac_macos_gl_wrapper_for_token(0xFE100080u) != NULL);
	assert(mac_macos_gl_wrapper_for_token(0xFEFFFFFFu) == NULL);
	assert(mac_macos_gl_native_proc("glGetString") == NULL);
	mac_guest_import_bridge proc_bridge =
		mac_macos_gl_host_import_bridge("guest_gl_get_proc_address");
	assert(proc_bridge != NULL);
	assert(mac_macos_gl_host_import_bridge("halo_gl41_copy_image_2d") != NULL);
	assert(mac_macos_gl_host_import_bridge("host_gl_get_string") != NULL);
	assert(mac_macos_gl_host_import_bridge("unknown_gl_import") == NULL);
	memcpy(guest_backing + (import_esp + 4u - TEST_GUEST_BASE), &proc_name_va, sizeof(proc_name_va));
	proc_bridge();
	assert(g_eax == 0xFE100080u && import_esp == TEST_GUEST_BASE + 0x404u);
	version_va = mac_macos_gl_return_guest_pointer("glGetString", "4.1 test",
		get_string_args, 1);
	assert(version_va != 0);
	assert(strcmp((const char *)mac_guest_address_resolve(version_va, sizeof("4.1 test")),
		"4.1 test") == 0);
	mac_macos_gl_native_proc_clear();
	return 0;
}
