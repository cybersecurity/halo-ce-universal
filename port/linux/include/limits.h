/*
LIMITS.H

Host <limits.h> with MSVC's 32-bit long, for the 64-bit build (HALO_64BIT).

The game's long is 32 bits (tools/lp64_rewrite.py spells it int), so
LONG_MAX and friends must be too: `node_index & LONG_MAX` strips a leaf flag
only if LONG_MAX is 0x7fffffff. cseries.h defines LONG_MAX, LONG_MIN,
CHAR_MAX and CHAR_MIN as enumerators, which a macro would break (whichever
header comes first), so the game sees no macros for those; the platform
layer, which doesn't include cseries.h, gets 32-bit macros.
*/

#ifndef __HALO_MODERN_LIMITS_H
#define __HALO_MODERN_LIMITS_H

#include_next <limits.h>

#ifdef HALO_64BIT
#undef LONG_MAX
#undef LONG_MIN
#undef ULONG_MAX
#undef CHAR_MAX
#undef CHAR_MIN
#define ULONG_MAX 0xffffffffU

#ifdef HALO_LINUX_PLATFORM_LAYER
#define LONG_MAX 2147483647
#define LONG_MIN (-2147483647 - 1)
#define CHAR_MAX 127
#define CHAR_MIN (-128)
#endif
#endif

#endif
