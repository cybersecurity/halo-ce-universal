/* Compile the POSIX file backend under a private host namespace. The original
 * names are occupied by AOT guest-import trampolines in the final executable. */
#include "posix_backend_names.h"
#include "../linux/src/posix_files.c"
