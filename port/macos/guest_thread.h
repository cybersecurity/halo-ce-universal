#ifndef HALO_MACOS_GUEST_THREAD_H
#define HALO_MACOS_GUEST_THREAD_H

#include <stddef.h>
#include <stdint.h>

/* Runtime operations supplied by the AOT executable. Stack allocation uses
 * the installed guest allocator; `initialize` runs on the new pthread and
 * initializes that thread's recomp TLS/register/TIB state; `dispatch` resolves
 * and invokes translated guest code (never casts a guest VA to a native
 * function pointer). */
struct mac_guest_thread_runtime
{
	void *context;
	void (*initialize)(void *context, uint32_t guest_thread_va, uint32_t guest_esp);
	void (*dispatch)(void *context, uint32_t guest_function_va);
};

/* Starts the guest's translated thread-start routine on a native pthread.
 * Returns 0 or a POSIX error number, matching the Android host contract. */
int mac_guest_thread_create(const struct mac_guest_thread_runtime *runtime,
	uint32_t guest_thread_va, uint32_t guest_start_va, size_t stack_size);

/* Import-facing form. The host image installs the runtime callbacks and the
 * translated guest thread-start VA once before guest pthreads can be created. */
int mac_guest_thread_runtime_install(const struct mac_guest_thread_runtime *runtime,
	uint32_t guest_start_va);
int mac_host_thread_create(uint32_t guest_thread_va, uint32_t stack_size);

#endif
