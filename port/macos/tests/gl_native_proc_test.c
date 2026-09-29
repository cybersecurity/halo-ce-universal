#include "../gl_native_proc.h"

#include <assert.h>

int main(void)
{
	/* No SDL GL context is created by this fixture. The resolver must fail
	 * closed instead of returning a process-global or truncated pointer. */
	assert(mac_macos_gl_native_proc("glGetString") == 0);
	assert(mac_macos_gl_native_proc("") == 0);
	assert(mac_macos_gl_native_proc(0) == 0);
	mac_macos_gl_native_proc_clear();
	return 0;
}
