/* The game includes <StdDef.h>; Linux file names are case sensitive. On
 * case-insensitive macOS filesystems this lowercase shim aliases StdDef.h
 * itself, so skip this include directory to reach the target C library. */
#if defined(HALO_MACOS)
#include_next <stddef.h>
#else
#include <stddef.h>
#endif
