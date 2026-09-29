#include "guest_address.h"
#include "guest_import_registry.h"
#include "host_imports.h"
#include "host_services.h"
#include "recomp_types.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define TEST_BASE UINT32_C(0x62000000)
#define TEST_SIZE (16u * 1024u)

_Thread_local uint32_t g_esp;
_Thread_local uint32_t g_eax;
_Thread_local uint32_t g_edx;

int main(void)
{
	unsigned char arena[TEST_SIZE] = { 0 };
	uint32_t return_va = UINT32_C(0x12345678);
	uint64_t syscall_frame[7] = { 172, 0, 0, 0, 0, 0, 0 };
	const char *manifest[] = { "host_get_tp", "host_set_tp", "host_syscall" };
	const mac_guest_import_provider providers[] = { mac_macos_host_import_bridge };
	mac_guest_function bridge;

	assert(mac_guest_address_register(TEST_BASE, arena, sizeof(arena)) == 0);
	assert(mac_guest_host_imports_install_recomp_tls() == 0);
	assert(mac_guest_import_register_manifest(manifest, 3, providers, 1, NULL) == 0);
	g_esp = TEST_BASE + 0x100;
	memcpy(arena + 0x100, &return_va, sizeof(return_va));
	memcpy(arena + 0x104, syscall_frame, sizeof(syscall_frame));

	bridge = mac_guest_import_resolve(mac_guest_import_token("host_syscall"));
	assert(bridge != NULL);
	bridge();
	assert(g_esp == TEST_BASE + 0x104); /* cdecl callee pops return only */
	assert(g_eax == (uint32_t)getpid());
	assert(g_edx == 0);

	g_esp = TEST_BASE + 0x200;
	memcpy(arena + 0x200, &return_va, sizeof(return_va));
	*(uint32_t *)(void *)(arena + 0x204) = UINT32_C(0x76543210);
	bridge = mac_guest_import_resolve(mac_guest_import_token("host_set_tp"));
	bridge();
	assert(mac_host_get_guest_tp() == UINT32_C(0x76543210));
	assert(g_esp == TEST_BASE + 0x204);

	g_esp = TEST_BASE + 0x300;
	memcpy(arena + 0x300, &return_va, sizeof(return_va));
	bridge = mac_guest_import_resolve(mac_guest_import_token("host_get_tp"));
	bridge();
	assert(g_eax == UINT32_C(0x76543210));
	assert(g_esp == TEST_BASE + 0x304);

	assert(mac_macos_host_import_bridge("host_sdl_gl_make_current") != NULL);
	assert(mac_macos_host_import_bridge("host_sdl_open_audio_stream") != NULL);
	assert(mac_macos_host_import_bridge("host_memory_watch_generation") == NULL);
	assert(mac_macos_host_import_bridge("host_import_that_does_not_exist") == NULL);

	mac_guest_import_reset();
	mac_guest_address_reset();
	puts("host_imports_test: ok");
	return 0;
}
