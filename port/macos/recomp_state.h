#ifndef HALO_MACOS_RECOMP_STATE_H
#define HALO_MACOS_RECOMP_STATE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Install the one image-wide linear guest-memory offset and executable VA
 * bounds returned by mac_guest_image_load/the ELF program headers. */
int mac_recomp_state_set_image(ptrdiff_t memory_offset, uint32_t code_lo,
	uint32_t code_hi);

/* Reset the translator's per-native-thread x86/x87/SSE state before entering
 * a translated guest thread. guest_esp is the guest stack pointer the runtime
 * has prepared; fs_base is a guest VA, or zero when no guest FS/TIB is used. */
void mac_recomp_state_initialize_thread(uint32_t guest_esp, uint32_t fs_base);

#ifdef __cplusplus
}
#endif

#endif
