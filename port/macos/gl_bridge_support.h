#ifndef HALO_MACOS_GL_BRIDGE_SUPPORT_H
#define HALO_MACOS_GL_BRIDGE_SUPPORT_H

#include <stddef.h>
#include <stdint.h>

/* Used by generated i686 cdecl wrappers in macos_gl_token_wrappers.c. */
void *mac_macos_gl_guest_pointer(const char *name, unsigned arg_index,
	uint32_t guest_va, const uint32_t *raw_args, unsigned arg_count);
uint32_t mac_macos_gl_return_guest_pointer(const char *name,
	const void *host_pointer, const uint32_t *raw_args, unsigned arg_count);
void mac_macos_gl_bridge_fault(const char *name, const char *reason);
int mac_guest_allocate(size_t size, size_t alignment, uint32_t *guest_va_out);
uint32_t mac_macos_gl_token_for_guest_name(uint32_t guest_name_va);

/* Exposed for the focused fixture and shared byte-size checks. */
size_t mac_macos_gl_pixel_span(uint32_t width, uint32_t height,
	uint32_t format, uint32_t type, uint32_t alignment);

#endif
