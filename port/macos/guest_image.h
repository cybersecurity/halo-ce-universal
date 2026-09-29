#ifndef HALO_MACOS_GUEST_IMAGE_H
#define HALO_MACOS_GUEST_IMAGE_H

#include <stddef.h>
#include <stdint.h>

struct mac_guest_image
{
	void *arena_host_base;
	uint32_t entry_va;
	uint32_t code_lo_va;
	uint32_t code_hi_va;
	uint32_t image_end_va;
	uint32_t tls_image_va;
	size_t tls_file_size;
	size_t tls_memory_size;
	size_t tls_alignment;
	ptrdiff_t memory_offset;
};

/* Loads an ELF32/i386 ET_EXEC image into a registered high host arena. The
 * reserved host mapping backs the full 32-bit guest address space; PT_LOAD
 * segments below the synthetic-import window are copied and BSS is zeroed.
 * The 0x70000000 import segment is intentionally left unmapped for runtime
 * token dispatch. Returns 0 or -1 with a bounded diagnostic in error_out. */
int mac_guest_image_load(const void *elf_bytes, size_t elf_size,
	struct mac_guest_image *image_out, char *error_out, size_t error_capacity);

/* Look up a defined ELF32 symbol while the original ELF bytes are available.
 * Used for runtime roots such as __guest_start and __guest_thread_start. */
int mac_guest_image_find_symbol(const void *elf_bytes, size_t elf_size,
	const char *name, uint32_t *guest_va_out);
/* Data-symbol lookup stays separate from the function-root lookup. */
int mac_guest_image_find_object(const void *elf_bytes, size_t elf_size,
	const char *name, uint32_t object_size, uint32_t *guest_va_out);

#endif
