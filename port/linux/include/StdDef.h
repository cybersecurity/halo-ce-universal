/* the game includes <StdDef.h>; Linux file names are case sensitive. On a
case-insensitive file system (macOS) a plain <stddef.h> would find this file
again; include_next goes to the real one, as the other shims do. */
#include_next <stddef.h>
