#ifndef HALO_MEMORY_WATCH_TEST_PLATFORM_H
#define HALO_MEMORY_WATCH_TEST_PLATFORM_H

#include <stddef.h>

#ifndef TRUE
#define TRUE 1
#define FALSE 0
typedef int BOOL;
#endif

extern unsigned char halo_watch_test_arena[];
#define PLATFORM_CONTIGUOUS_BASE ((unsigned long)halo_watch_test_arena)
#define PLATFORM_CONTIGUOUS_SIZE 0x10000UL

BOOL platform_is_contiguous(const void *address);

#endif
