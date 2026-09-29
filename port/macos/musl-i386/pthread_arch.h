/* Darwin's native thread has its own FS/GS base. The translated i386 guest
 * stores musl's thread pointer through guest_thread.c/host_get_tp instead.
 * Keep the i386 signal-context EIP mapping while replacing only __get_tp. */
#include <stdint.h>

uintptr_t __guest_get_tp(void);

static inline uintptr_t __get_tp(void)
{
	return __guest_get_tp();
}

#define MC_PC gregs[REG_EIP]
