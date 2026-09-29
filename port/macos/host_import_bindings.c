#include "host_import_bindings.h"

#include "guest_import_registry.h"
#include "host_import_names.h"
#include "host_imports.h"
#include "host_services.h"
#include "gl_host_imports.h"
#include "posix_host_imports.h"

#include <string.h>

static size_t bound_count;
static size_t missing_count;

static const mac_guest_import_provider providers[] = {
	mac_macos_host_import_bridge,
	mac_macos_gl_host_import_bridge,
	mac_macos_posix_host_import_bridge,
};

int mac_macos_host_imports_register(void)
{
	if (mac_guest_host_imports_install_recomp_tls() != 0)
		return -1;
	bound_count = 0;
	missing_count = 0;
	for (size_t i = 0; i < MAC_GUEST_HOST_IMPORT_COUNT; ++i)
	{
		int found = 0;
		for (size_t p = 0; p < sizeof(providers) / sizeof(providers[0]); ++p)
			if (providers[p](mac_guest_host_import_names[i])) { found = 1; break; }
		if (found) ++bound_count;
		else
		{
			++missing_count;
			mac_host_logf(2, "host import has no Darwin provider: %s", mac_guest_host_import_names[i]);
		}
	}
	mac_host_logf(missing_count ? 2 : 1, "bound %zu/%u generated guest imports (%zu fail-closed)",
		bound_count, MAC_GUEST_HOST_IMPORT_COUNT, missing_count);
	return mac_guest_import_register_manifest(mac_guest_host_import_names,
		MAC_GUEST_HOST_IMPORT_COUNT, providers,
		sizeof(providers) / sizeof(providers[0]), NULL);
}

size_t mac_macos_host_imports_bound_count(void) { return bound_count; }
size_t mac_macos_host_imports_missing_count(void) { return missing_count; }
