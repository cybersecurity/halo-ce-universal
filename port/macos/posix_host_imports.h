#ifndef HALO_MACOS_POSIX_HOST_IMPORTS_H
#define HALO_MACOS_POSIX_HOST_IMPORTS_H

#include "guest_import_registry.h"

/* POSIX platform functions are compiled natively and entered through the
 * translated i386 cdecl stack. All guest pointers are checked before use. */
mac_guest_import_bridge mac_macos_posix_host_import_bridge(const char *name);

#endif
