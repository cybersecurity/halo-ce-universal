#ifndef HALO_MACOS_GL_NATIVE_PROC_H
#define HALO_MACOS_GL_NATIVE_PROC_H

#ifdef __cplusplus
extern "C" {
#endif

/* Resolve a native GL entry point for the SDL GL context current on this
 * thread. Returns NULL if there is no current context or SDL has no entry. */
void *mac_macos_gl_native_proc(const char *name);

/* Drop cached addresses when the owning SDL GL context is destroyed. */
void mac_macos_gl_native_proc_clear(void);

#ifdef __cplusplus
}
#endif

#endif
