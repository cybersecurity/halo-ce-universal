#include "../gl_bridge_support.h"
#include "../gl_host_imports.h"
#include "../gl_native_proc.h"
#include "../gl_token_registry.h"
#include "../guest_address.h"
#include "../guest_allocator.h"
#include "../guest_call.h"

#define GL_SILENCE_DEPRECATION 1
#define GL_GLEXT_PROTOTYPES 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <SDL3/SDL_opengl_glext.h>
#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern _Thread_local uint32_t g_eax;
extern _Thread_local uint32_t g_esp;

#define TEST_GUEST_BASE 0x10000u
#define TEST_GUEST_SIZE 0x10000u

static uint8_t guest_backing[TEST_GUEST_SIZE];
static uint32_t allocation_cursor = TEST_GUEST_BASE + 0x8000u;
static uint32_t import_esp = TEST_GUEST_BASE + 0x400u;

static int test_allocate(void *context, size_t size, size_t alignment,
	uint32_t *guest_va_out)
{
	(void)context;
	if (!size || !alignment || !guest_va_out)
		return -1;
	uint64_t address = ((uint64_t)allocation_cursor + alignment - 1) &
		~((uint64_t)alignment - 1);
	if (address + size > TEST_GUEST_BASE + TEST_GUEST_SIZE)
		return -1;
	*guest_va_out = (uint32_t)address;
	allocation_cursor = (uint32_t)(address + size);
	return 0;
}

static void test_release(void *context, uint32_t guest_va, size_t size)
{
	(void)context;
	(void)guest_va;
	(void)size;
}

uint32_t mac_guest_host_import_arg32(uint32_t index)
{
	uint32_t value = 0;
	assert(mac_guest_read(import_esp + 4u + index * 4u, &value, sizeof(value)) == 0);
	return value;
}

void mac_guest_host_import_return32(uint32_t value)
{
	g_eax = value;
	import_esp += 4;
}

void mac_guest_host_import_return_void(void)
{
	import_esp += 4;
}

void mac_host_abort(const char *message)
{
	(void)message;
	abort();
}

/* Coexists with the generated guest alias; invoking this from the native bridge
 * would reproduce the original recursive dispatch. */
void halo_gl41_copy_image_2d(void) { assert(!"native bridge called guest alias"); }

static void test_actual_copy_bridge(void)
{
 assert(SDL_Init(SDL_INIT_VIDEO));
 assert(SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4));
 assert(SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1));
 assert(SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE));
 SDL_Window *window = SDL_CreateWindow("Halo GL copy regression",64,64,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
 assert(window);
 SDL_GLContext context=SDL_GL_CreateContext(window); assert(context);
 mac_macos_gl_native_proc_clear();
 GLuint textures[2], previous_fbos[2];
 glGenTextures(2,textures); glGenFramebuffers(2,previous_fbos);
 unsigned char pixels[8*8*4], actual[sizeof pixels];
 for(unsigned i=0;i<sizeof pixels;i++) pixels[i]=(unsigned char)(i*17+3);
 for(unsigned t=0;t<2;t++) {
  glBindTexture(GL_TEXTURE_2D,textures[t]);
  glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,8,8,0,GL_RGBA,GL_UNSIGNED_BYTE,t?NULL:pixels);
  glTexImage2D(GL_TEXTURE_2D,1,GL_RGBA8,4,4,0,GL_RGBA,GL_UNSIGNED_BYTE,t?NULL:pixels);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
 }
 for(unsigned mip=0;mip<2;mip++) {
  glBindFramebuffer(GL_READ_FRAMEBUFFER,previous_fbos[0]);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER,previous_fbos[1]);
  glEnable(GL_SCISSOR_TEST); glScissor(0,0,1,1);
  const uint32_t api_va=TEST_GUEST_BASE+0x1000, state_va=TEST_GUEST_BASE+0x1100;
  memset(guest_backing+0x1000,0,44); memset(guest_backing+0x1100,0,8);
  uint32_t width=8>>mip, args[]={api_va,state_va,textures[0],mip,textures[1],mip,width,width};
  import_esp=TEST_GUEST_BASE+0x400;
  assert(mac_guest_write(import_esp+4,args,sizeof args)==0);
  mac_macos_gl_host_import_bridge("halo_gl41_copy_image_2d")();
  assert(g_eax==1 && import_esp==TEST_GUEST_BASE+0x404);
  GLint read,draw; glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&read); glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&draw);
  assert(read==(GLint)previous_fbos[0] && draw==(GLint)previous_fbos[1] && glIsEnabled(GL_SCISSOR_TEST));
  glBindFramebuffer(GL_READ_FRAMEBUFFER,previous_fbos[0]);
  glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,textures[1],mip);
  glReadBuffer(GL_COLOR_ATTACHMENT0); assert(glCheckFramebufferStatus(GL_READ_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE);
  glReadPixels(0,0,width,width,GL_RGBA,GL_UNSIGNED_BYTE,actual);
  assert(memcmp(actual,pixels,width*width*4)==0 && glGetError()==GL_NO_ERROR);
 }
 /* A smaller non-square region must preserve every destination pixel outside it. */
 unsigned char expected[sizeof pixels]; memset(expected,0xa5,sizeof expected);
 glBindTexture(GL_TEXTURE_2D,textures[1]);
 glTexSubImage2D(GL_TEXTURE_2D,0,0,0,8,8,GL_RGBA,GL_UNSIGNED_BYTE,expected);
 glBindFramebuffer(GL_READ_FRAMEBUFFER,previous_fbos[0]);
 glBindFramebuffer(GL_DRAW_FRAMEBUFFER,previous_fbos[1]);
 glEnable(GL_SCISSOR_TEST); glScissor(7,7,1,1);
 uint32_t subargs[]={TEST_GUEST_BASE+0x1000,TEST_GUEST_BASE+0x1100,textures[0],0,textures[1],0,3,2};
 import_esp=TEST_GUEST_BASE+0x400;
 assert(mac_guest_write(import_esp+4,subargs,sizeof subargs)==0);
 mac_macos_gl_host_import_bridge("halo_gl41_copy_image_2d")();
 assert(g_eax==1 && import_esp==TEST_GUEST_BASE+0x404);
 GLint subread,subdraw; glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&subread); glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&subdraw);
 assert(subread==(GLint)previous_fbos[0] && subdraw==(GLint)previous_fbos[1] && glIsEnabled(GL_SCISSOR_TEST));
 glBindFramebuffer(GL_READ_FRAMEBUFFER,previous_fbos[0]);
 glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,textures[1],0);
 glReadBuffer(GL_COLOR_ATTACHMENT0);
 glReadPixels(0,0,8,8,GL_RGBA,GL_UNSIGNED_BYTE,actual);
 for(unsigned row=0;row<2;row++) memcpy(expected+row*8*4,pixels+row*8*4,3*4);
 assert(memcmp(actual,expected,sizeof expected)==0 && glGetError()==GL_NO_ERROR);
 glDeleteTextures(2,textures);glDeleteFramebuffers(2,previous_fbos);
 SDL_GL_DestroyContext(context);SDL_DestroyWindow(window);SDL_Quit();
 puts("Actual guest import/native GL4.1 pixels+mip+state+alias regression passed");
}

int main(void)
{
	const struct mac_guest_allocator allocator = {
		NULL, test_allocate, test_release
	};
	const uint32_t known_name_va = TEST_GUEST_BASE + 0x100u;
	const uint32_t unknown_name_va = TEST_GUEST_BASE + 0x120u;
	const uint32_t proc_name_va = TEST_GUEST_BASE + 0x140u;
	const uint32_t get_string_args[] = { 0x1F02u };
	uint32_t version_va;

	assert(mac_guest_address_register(TEST_GUEST_BASE, guest_backing,
		sizeof(guest_backing)) == 0);
	memcpy(guest_backing + (known_name_va - TEST_GUEST_BASE), "glViewport", sizeof("glViewport"));
	memcpy(guest_backing + (unknown_name_va - TEST_GUEST_BASE), "glNoSuchCall", sizeof("glNoSuchCall"));
	memcpy(guest_backing + (proc_name_va - TEST_GUEST_BASE), "glViewport", sizeof("glViewport"));
	assert(mac_guest_allocator_install(&allocator) == 0);
	assert(mac_macos_gl_token_for_guest_name(known_name_va) == 0xFE100080u);
	assert(mac_macos_gl_token_for_guest_name(unknown_name_va) == 0);
	assert(mac_macos_gl_wrapper_for_token(0xFE100080u) != NULL);
	assert(mac_macos_gl_wrapper_for_token(0xFEFFFFFFu) == NULL);
	assert(mac_macos_gl_native_proc("glGetString") == NULL);
	mac_guest_import_bridge proc_bridge =
		mac_macos_gl_host_import_bridge("guest_gl_get_proc_address");
	assert(proc_bridge != NULL);
	assert(mac_macos_gl_host_import_bridge("halo_gl41_copy_image_2d") != NULL);
	assert(mac_macos_gl_host_import_bridge("host_gl_get_string") != NULL);
	assert(mac_macos_gl_host_import_bridge("unknown_gl_import") == NULL);
	memcpy(guest_backing + (import_esp + 4u - TEST_GUEST_BASE), &proc_name_va, sizeof(proc_name_va));
	proc_bridge();
	assert(g_eax == 0xFE100080u && import_esp == TEST_GUEST_BASE + 0x404u);
	version_va = mac_macos_gl_return_guest_pointer("glGetString", "4.1 test",
		get_string_args, 1);
	assert(version_va != 0);
	assert(strcmp((const char *)mac_guest_address_resolve(version_va, sizeof("4.1 test")),
		"4.1 test") == 0);
	mac_macos_gl_native_proc_clear();
 test_actual_copy_bridge();
	return 0;
}
