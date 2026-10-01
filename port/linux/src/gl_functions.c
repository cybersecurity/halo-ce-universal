/*
GL_FUNCTIONS.C

Run-time resolution of the OpenGL entry points listed in gl.h.
*/

#include "platform.h"
#define GL_FUNCTIONS_DEFINE
#include "gl.h"

#include <SDL3/SDL.h>
#include <string.h>

#define GL_DEFINE_FUNCTION(name) __typeof__(&name) halo_##name;
GL_FUNCTIONS(GL_DEFINE_FUNCTION)

int gl_functions_load(void)
{
	int success = TRUE;

/* newer than OpenGL 4.1 (macOS's newest); callers check for NULL */
#define GL_OPTIONAL_FUNCTION(name) \
	(!strcmp(#name, "glClipControl") || !strcmp(#name, "glCopyImageSubData") || \
		!strcmp(#name, "glDebugMessageCallback") || !strcmp(#name, "glBufferStorage"))
#define GL_LOAD_FUNCTION(name) \
	halo_##name = (__typeof__(halo_##name))SDL_GL_GetProcAddress(#name); \
	if (!halo_##name && !GL_OPTIONAL_FUNCTION(name)) \
	{ \
		platform_log("OpenGL function %s is unavailable", #name); \
		success = FALSE; \
	}
	GL_FUNCTIONS(GL_LOAD_FUNCTION)
#undef GL_LOAD_FUNCTION
	return success;
}
