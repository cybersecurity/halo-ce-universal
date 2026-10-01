#ifdef HALO_64BIT
/* the game includes <StdDef.h>; this must not find itself on a case-insensitive file system */
#include_next <stddef.h>
#else
/* the game includes <StdDef.h>; Linux file names are case sensitive */
#include <stddef.h>
#endif
