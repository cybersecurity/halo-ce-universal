/* the parts of port/linux/src/platform.h raytrace_test.c's files use */
#ifndef TEST_PLATFORM_H
#define TEST_PLATFORM_H
#include <stdlib.h> /* as port/linux/src/platform.h has it: getenv, atof */
#define TRUE 1
#define FALSE 0
void platform_log(const char *format, ...);
#endif
