#include "../gl_native_proc.h"
#include <SDL3/SDL.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static SDL_GLContext current;
static unsigned lookups;
static void first(void) {}
static void second(void) {}
SDL_GLContext SDL_GL_GetCurrentContext(void) { return current; }
SDL_FunctionPointer SDL_GL_GetProcAddress(const char *name)
{
	++lookups;
	if (!strcmp(name, "glMissing")) return NULL;
	return current == (SDL_GLContext)(uintptr_t)1 ? first : second;
}
const char *SDL_GetError(void) { return "fixture missing entry point"; }

int main(void)
{
	assert(!mac_macos_gl_native_proc("glGetString"));
	assert(!mac_macos_gl_native_proc(""));
	assert(!mac_macos_gl_native_proc(NULL));
	current = (SDL_GLContext)(uintptr_t)1;
	char name[64];
	for (unsigned i = 0; i < 100; ++i)
	{
		snprintf(name, sizeof(name), "glFixture%u", i);
		assert(mac_macos_gl_native_proc(name) == (void *)first);
	}
	assert(lookups == 100);
	/* Independently allocated names must hit the same cache key. */
	for (unsigned i = 0; i < 100; ++i)
	{
		snprintf(name, sizeof(name), "glFixture%u", i);
		assert(mac_macos_gl_native_proc(name) == (void *)first);
	}
	assert(lookups == 100);
	current = (SDL_GLContext)(uintptr_t)2;
	assert(mac_macos_gl_native_proc("glFixture0") == (void *)second);
	assert(lookups == 101);
	current = (SDL_GLContext)(uintptr_t)1;
	assert(mac_macos_gl_native_proc("glFixture0") == (void *)first);
	assert(lookups == 101);
	assert(!mac_macos_gl_native_proc("glMissing"));
	assert(!mac_macos_gl_native_proc("glMissing"));
	assert(lookups == 102);
	mac_macos_gl_native_proc_clear();
	assert(mac_macos_gl_native_proc("glFixture0") == (void *)first);
	assert(lookups == 103);
	puts("gl_native_proc_cache_test: repeat names, context isolation, missing procedures and clear passed");
	return 0;
}
