#ifndef HALO_MACOS_GUEST_IMPORT_REGISTRY_H
#define HALO_MACOS_GUEST_IMPORT_REGISTRY_H

#include <stddef.h>
#include <stdint.h>

typedef void (*mac_guest_function)(void);
typedef void (*mac_guest_import_bridge)(void);
typedef mac_guest_import_bridge (*mac_guest_import_provider)(const char *name);

/* Synthetic guest VA subrange reserved for native host-import tokens. The
 * AOT manual resolver must check this registry before the Xbox kernel thunk
 * resolver. Each token is four-byte spaced and maps to a unique host wrapper. */
#define MAC_GUEST_IMPORT_TOKEN_BASE UINT32_C(0x70000000)
#define MAC_GUEST_IMPORT_TOKEN_STRIDE 16u
#define MAC_GUEST_IMPORT_CAPACITY 128u

int mac_guest_import_register(const char *name, mac_guest_import_bridge bridge,
	uint32_t *token_out);
/* Registers one already sorted final ELF import manifest by asking each
 * provider for a bridge by name. A supplied fallback is used for unknown
 * imports and must fail loudly when invoked. */
int mac_guest_import_register_manifest(const char *const *names, size_t count,
	const mac_guest_import_provider *providers, size_t provider_count,
	mac_guest_import_bridge unsupported_fallback);
uint32_t mac_guest_import_token(const char *name);
mac_guest_function mac_guest_import_resolve(uint32_t token);
/* Dispatch the bridge for one exact token; used by manifest-generated
 * strong `sub_<VA>` aliases for direct AOT calls. */
void mac_guest_import_dispatch_token(uint32_t token);
size_t mac_guest_import_count(void);
void mac_guest_import_reset(void);

#endif
