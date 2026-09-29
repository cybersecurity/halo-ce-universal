#ifndef HALO_MACOS_HOST_IMPORTS_H
#define HALO_MACOS_HOST_IMPORTS_H

#include "guest_import_registry.h"

#include <stdint.h>

/* Accessors for the active translator TLS state. Calls are made on the guest
 * thread, so these must read/write that thread's current guest register set. */
struct mac_guest_import_registers
{
	void *context;
	uint32_t (*get_esp)(void *context);
	void (*set_esp)(void *context, uint32_t value);
	void (*set_eax)(void *context, uint32_t value);
	void (*set_edx)(void *context, uint32_t value);
};

int mac_guest_host_import_registers_install(const struct mac_guest_import_registers *registers);
/* Production adapter compiled with the generated recomp_types.h globals. */
int mac_guest_host_imports_install_recomp_tls(void);
uint32_t mac_guest_host_import_esp(void);
uint32_t mac_guest_host_import_arg32(uint32_t index);
uint64_t mac_guest_host_import_arg64_words(uint32_t low_index);
void mac_guest_host_import_return32(uint32_t value);
void mac_guest_host_import_return64(int64_t value);
void mac_guest_host_import_return_void(void);
/* Returns a cdecl guest bridge for implemented host_* imports, or NULL. The
 * sorted manifest owner binds these in deterministic token order. */
mac_guest_import_bridge mac_macos_host_import_bridge(const char *name);

#endif
