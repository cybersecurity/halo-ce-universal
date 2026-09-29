#ifndef HALO_MACOS_GL_TOKEN_REGISTRY_H
#define HALO_MACOS_GL_TOKEN_REGISTRY_H

#include <stdint.h>

typedef void (*mac_macos_gl_guest_function)(void);

/* Generated procedure-name/token registry for i686 GL function pointers. */
uint32_t mac_macos_gl_token_for_name(const char *name);
mac_macos_gl_guest_function mac_macos_gl_wrapper_for_token(uint32_t token);

#endif
