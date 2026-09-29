#include "host_imports.h"

#include "recomp_types.h"

static uint32_t active_esp(void *context)
{
	(void)context;
	return g_esp;
}

static void set_esp(void *context, uint32_t value)
{
	(void)context;
	g_esp = value;
}

static void set_eax(void *context, uint32_t value)
{
	(void)context;
	g_eax = value;
}

static void set_edx(void *context, uint32_t value)
{
	(void)context;
	g_edx = value;
}

int mac_guest_host_imports_install_recomp_tls(void)
{
	const struct mac_guest_import_registers access = {
		NULL, active_esp, set_esp, set_eax, set_edx
	};
	return mac_guest_host_import_registers_install(&access);
}
