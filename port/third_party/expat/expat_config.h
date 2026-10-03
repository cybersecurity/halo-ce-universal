/* Expat's build configuration for the ports (README.md): the menus' XML
files only, so no DTDs, no namespaces and no general entities. */

#ifndef EXPAT_CONFIG_H
#define EXPAT_CONFIG_H 1

#define BYTEORDER 1234
#define HAVE_MEMMOVE 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define STDC_HEADERS 1

#define XML_CONTEXT_BYTES 1024
#define XML_GE 0

/* The hash salt only guards against hash flooding, which small local files
can't do; Windows takes it from rand_s, and the others from the time */
#ifndef _WIN32
#define XML_POOR_ENTROPY 1
#endif

#endif
