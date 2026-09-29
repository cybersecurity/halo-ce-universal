#ifndef HALO_MACOS_HOST_IMPORT_BINDINGS_H
#define HALO_MACOS_HOST_IMPORT_BINDINGS_H

#include <stddef.h>

/* Install the generated final-ELF import manifest in exact sorted order. */
int mac_macos_host_imports_register(void);
size_t mac_macos_host_imports_bound_count(void);
size_t mac_macos_host_imports_missing_count(void);

#endif
