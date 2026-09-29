#ifndef HALO_MACOS_GUEST_ERRNO_H
#define HALO_MACOS_GUEST_ERRNO_H

/* Install the exact hash-matched translated musl __errno_location accessor.
 * The writer discovers guest TLS through that accessor, never host TLS offsets. */
int mac_guest_errno_install(void (*location_accessor)(void));
int mac_guest_errno_set_native(int native_error);
#endif
