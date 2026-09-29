/*
 * Helpers for host callbacks entered from translated 32-bit x86 functions.
 * At callback entry the translated guest ESP points at the 32-bit return VA;
 * cdecl/stdcall arguments follow it in 4-byte slots. Host functions must use
 * these helpers instead of treating guest VAs as native pointers.
 */

#ifndef HALO_MACOS_GUEST_CALL_H
#define HALO_MACOS_GUEST_CALL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int mac_guest_read(uint32_t guest_address, void *destination, size_t size);
int mac_guest_write(uint32_t guest_address, const void *source, size_t size);
int mac_guest_stack_return(uint32_t guest_esp, uint32_t *return_va_out);
int mac_guest_stack_arg32(uint32_t guest_esp, uint32_t argument_index,
	uint32_t *value_out);
int mac_guest_stack_arg64(uint32_t guest_esp, uint32_t argument_index,
	uint64_t *value_out);
int mac_guest_stack_arg_span(uint32_t guest_esp, uint32_t argument_index,
	size_t size, void **span_out);

/* Compute guest ESP after a translated host callback returns. `callee_bytes`
 * is zero for cdecl, or the byte count consumed by stdcall. */
int mac_guest_stack_after_return(uint32_t guest_esp, uint32_t callee_bytes,
	uint32_t *new_esp_out);

#ifdef __cplusplus
}
#endif

#endif
