#include <SDL3/SDL_opengl.h>

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "guest_address.h"

typedef void (*gl_guest_wrapper)(void);
extern uint32_t mac_macos_gl_token_for_name(const char *name);
extern gl_guest_wrapper mac_macos_gl_wrapper_for_token(uint32_t token);

_Thread_local uint32_t g_eax;
_Thread_local uint32_t g_esp;
_Thread_local uint32_t g_ebp;
ptrdiff_t g_xbox_mem_offset;
static uint32_t guest_stack[16];
static GLint observed_viewport[4];
static uint32_t observed_gl_string_name;

static void APIENTRY fake_glViewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
	observed_viewport[0] = x;
	observed_viewport[1] = y;
	observed_viewport[2] = width;
	observed_viewport[3] = height;
}

static GLenum APIENTRY fake_glGetError(void)
{
	return GL_INVALID_ENUM;
}

static const GLubyte *APIENTRY fake_glGetString(GLenum name)
{
	observed_gl_string_name = name;
	return (const GLubyte *)"OpenGL 4.1 wrapper fixture";
}

void *mac_macos_gl_native_proc(const char *name)
{
	if (strcmp(name, "glViewport") == 0) return (void *)fake_glViewport;
	if (strcmp(name, "glGetError") == 0) return (void *)fake_glGetError;
	if (strcmp(name, "glGetString") == 0) return (void *)fake_glGetString;
	return NULL;
}

void *mac_macos_gl_guest_pointer(const char *name, unsigned arg_index,
	uint32_t guest_va, const uint32_t *raw_args, unsigned arg_count)
{
	(void)name; (void)arg_index; (void)guest_va; (void)raw_args; (void)arg_count;
	return NULL;
}

uint32_t mac_macos_gl_return_guest_pointer(const char *name, const void *host_pointer,
	const uint32_t *raw_args, unsigned arg_count)
{
	assert(strcmp(name, "glGetString") == 0);
	assert(host_pointer != NULL && raw_args != NULL && arg_count == 1);
	assert(raw_args[0] == GL_VERSION);
	return 0x2000;
}

int mac_guest_allocate(size_t size, size_t alignment, uint32_t *guest_va_out)
{
	(void)size; (void)alignment; (void)guest_va_out;
	return 0;
}

void mac_macos_gl_bridge_fault(const char *name, const char *reason)
{
	(void)name; (void)reason;
	assert(!"unexpected bridge fault");
}

int main(void)
{
	gl_guest_wrapper wrapper;
	uint32_t token = mac_macos_gl_token_for_name("glViewport");
	guest_stack[0] = UINT32_C(0xDEADBEEF); /* caller return VA */
	assert(mac_guest_address_register(0x1000, guest_stack, sizeof(guest_stack)) == 0);
	assert(token == 0xFE100080u);
	wrapper = mac_macos_gl_wrapper_for_token(token);
	assert(wrapper != NULL);
	g_esp = 0x1000;
	guest_stack[1] = (uint32_t)-320;
	guest_stack[2] = (uint32_t)-240;
	guest_stack[3] = 1280;
	guest_stack[4] = 720;
	wrapper();
	assert(g_esp == 0x1004);
	assert(observed_viewport[0] == -320 && observed_viewport[1] == -240 &&
		observed_viewport[2] == 1280 && observed_viewport[3] == 720);
	wrapper = mac_macos_gl_wrapper_for_token(mac_macos_gl_token_for_name("glGetError"));
	assert(wrapper != NULL);
	g_esp = 0x1000;
	wrapper();
	assert(g_eax == GL_INVALID_ENUM && g_esp == 0x1004);
	wrapper = mac_macos_gl_wrapper_for_token(mac_macos_gl_token_for_name("glGetString"));
	assert(wrapper != NULL);
	g_esp = 0x1000;
	guest_stack[1] = GL_VERSION;
	observed_gl_string_name = 0;
	wrapper();
	assert(observed_gl_string_name == GL_VERSION);
	assert(g_eax == 0x2000 && g_esp == 0x1004);
	mac_guest_address_reset();
	return 0;
}
