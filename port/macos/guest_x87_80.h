#ifndef HALO_MACOS_GUEST_X87_80_H
#define HALO_MACOS_GUEST_X87_80_H

#include <stdint.h>

/*
 * Convert the little-endian 10-byte x87 binary80 memory representation used
 * by the guest. The helpers do not cast through host long double (which is
 * binary64 on Apple Silicon). Loading rounds finite values to binary64 using
 * the supplied x87 RC bits (nearest-even, down, up, toward-zero). This models
 * the runtime's double-backed x87 registers and therefore loses precision
 * for values that require more than 53 significand bits. Invalid x87
 * encodings fail closed. Return 0 on success and -1 on invalid arguments or
 * unsupported encodings.
 */
int mac_guest_x87_80_load(const void *bytes10, unsigned rounding_mode, double *out);
int mac_guest_x87_80_store(void *bytes10, double value);

#endif
