#ifndef HALO_MACOS_GUEST_CALLBACK_H
#define HALO_MACOS_GUEST_CALLBACK_H

#include "guest_thread.h"

#include <stddef.h>
#include <stdint.h>

struct mac_guest_callback_context;

/* Install the same translated dispatcher used by native guest threads plus
 * the ELF VA of __guest_thread_attach. Callback workers run attach on their
 * own native thread before dispatching any guest callback. */
int mac_guest_callback_runtime_install(const struct mac_guest_thread_runtime *runtime,
	uint32_t guest_thread_attach_va);

/* These functions are called only from the native worker thread that will run
 * callbacks. It gets a guest stack and guest pthread/TLS through the guest's
 * own __guest_thread_attach routine. */
int mac_guest_callback_context_create_current(size_t guest_stack_size,
	struct mac_guest_callback_context **context_out);
int mac_guest_callback_invoke(struct mac_guest_callback_context *context,
	uint32_t guest_function_va, const uint32_t *arguments, size_t argument_count);
void mac_guest_callback_context_destroy_current(struct mac_guest_callback_context *context);

#endif
