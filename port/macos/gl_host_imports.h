#ifndef HALO_MACOS_GL_HOST_IMPORTS_H
#define HALO_MACOS_GL_HOST_IMPORTS_H

#include "guest_import_registry.h"

/* Return the cdecl host bridge for one Android renderer host_gl_* import, or
 * NULL. The global host-import registry owns deterministic token ordering. */
mac_guest_import_bridge mac_macos_gl_host_import_bridge(const char *name);

#endif
