#ifndef HALO_MACOS_HOST_SYSCALL_H
#define HALO_MACOS_HOST_SYSCALL_H

#include <stdint.h>

/* Optional VM operations are supplied by the executable's guest allocator.
 * The callbacks must preserve one linear guest VA backing and account for
 * Darwin's host page size. Unsupported file-backed mappings should fail. */
struct mac_guest_vm_ops
{
	void *context;
	int64_t (*map)(void *context, uint32_t address, uint32_t size,
		int protection, uint32_t flags, int fd, uint64_t offset,
		uint32_t *mapped_guest_va_out);
	int64_t (*unmap)(void *context, uint32_t address, uint32_t size);
	int64_t (*protect)(void *context, uint32_t address, uint32_t size,
		int protection);
};

int mac_guest_vm_ops_install(const struct mac_guest_vm_ops *operations);
/* The source-built ILP32 guest forwards syscall numbers from the arm64_32
 * Linux table plus its shared Linux time64 extension (clock_gettime64 403).
 * Pointers remain 32-bit; 403 writes two 64-bit fields, while legacy clock
 * call 113 writes two 32-bit fields. Returns
 * Linux convention: result or negative guest errno; unknown calls are ENOSYS. */
int64_t mac_host_arm64_32_syscall(uint64_t number, const uint64_t arguments[6]);
/* Reads number+six 64-bit argument slots at the checked i386 guest stack. */
int64_t mac_host_arm64_32_syscall_from_stack(uint32_t guest_esp);

#endif
