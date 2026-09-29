#include "gl_host_imports.h"

#include "guest_address.h"
#include "guest_call.h"
#include "host_imports.h"
#include "gl41_compat.h"
#include "gl_bridge_support.h"
#include "gl_native_proc.h"
#include "host_services.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_FENCE_SLOTS 8u
#define GUEST_STRING_LIMIT 4096u

typedef const GLubyte *(APIENTRYP gl_get_string_fn)(GLenum);
typedef const GLubyte *(APIENTRYP gl_get_string_i_fn)(GLenum, GLuint);
typedef void (APIENTRYP gl_get_integer_fn)(GLenum, GLint *);
typedef void (APIENTRYP gl_get_buffer_sub_data_fn)(GLenum, GLintptr, GLsizeiptr, void *);
typedef void *(APIENTRYP gl_map_buffer_range_fn)(GLenum, GLintptr, GLsizeiptr, GLbitfield);
typedef GLboolean (APIENTRYP gl_unmap_buffer_fn)(GLenum);
typedef void (APIENTRYP gl_buffer_sub_data_fn)(GLenum, GLintptr, GLsizeiptr, const void *);
typedef GLsync (APIENTRYP gl_fence_sync_fn)(GLenum, GLbitfield);
typedef GLenum (APIENTRYP gl_client_wait_sync_fn)(GLsync, GLbitfield, GLuint64);
typedef void (APIENTRYP gl_delete_sync_fn)(GLsync);

static GLsync frame_fences[FRAME_FENCE_SLOTS];

static void import_fail(const char *name, const char *reason)
{
	char message[256];
	snprintf(message, sizeof(message), "OpenGL host import %s: %s", name, reason);
	mac_host_abort(message);
}

static uint32_t argument(uint32_t index, const char *name)
{
	(void)name;
	return mac_guest_host_import_arg32(index);
}

static void cdecl_return(const char *name)
{
	(void)name;
	mac_guest_host_import_return_void();
}

static void cdecl_return32(uint32_t value)
{
	mac_guest_host_import_return32(value);
}

static const char *guest_c_string(uint32_t guest_va, const char *name)
{
	for (uint32_t length = 0; length < GUEST_STRING_LIMIT; length++)
	{
		uint64_t address = (uint64_t)guest_va + length;
		const char *character;
		if (address > UINT32_MAX || !(character = mac_guest_address_resolve((uint32_t)address, 1)))
			return NULL;
		if (!*character)
			return mac_guest_address_resolve(guest_va, (size_t)length + 1);
	}
	(void)name;
	return NULL;
}

static void import_get_string(void)
{
	const char *name = "host_gl_get_string";
	GLenum pname = argument(0, name);
	int32_t index = (int32_t)argument(1, name);
	uint32_t buffer_va = argument(2, name);
	uint32_t capacity = argument(3, name);
	char *buffer;
	const GLubyte *text = NULL;
	if (!capacity)
		goto done;
	buffer = mac_guest_address_resolve(buffer_va, capacity);
	if (!buffer)
		import_fail(name, "output string buffer is unmapped");
	buffer[0] = '\0';
	if (index >= 0)
	{
		gl_get_string_i_fn get_string_i = (gl_get_string_i_fn)mac_macos_gl_native_proc("glGetStringi");
		if (!get_string_i)
			import_fail(name, "glGetStringi is unavailable");
		text = get_string_i(pname, (GLuint)index);
	}
	else
	{
		gl_get_string_fn get_string = (gl_get_string_fn)mac_macos_gl_native_proc("glGetString");
		if (!get_string)
			import_fail(name, "glGetString is unavailable");
		text = get_string(pname);
	}
	if (text)
	{
		uint32_t offset = 0;
		while (offset + 1 < capacity && text[offset])
		{
			buffer[offset] = (char)text[offset];
			offset++;
		}
		buffer[offset] = '\0';
	}
done:
	cdecl_return(name);
}

static void import_has_extension(void)
{
	const char *name = "host_gl_has_extension";
	const char *wanted = guest_c_string(argument(0, name), name);
	GLint count = 0;
	int found = 0;
	gl_get_integer_fn get_integer;
	gl_get_string_i_fn get_string_i;
	if (!wanted)
		import_fail(name, "extension name is unmapped or unterminated");
	get_integer = (gl_get_integer_fn)mac_macos_gl_native_proc("glGetIntegerv");
	get_string_i = (gl_get_string_i_fn)mac_macos_gl_native_proc("glGetStringi");
	if (!get_integer || !get_string_i)
		import_fail(name, "extension enumeration requires glGetIntegerv and glGetStringi");
	get_integer(GL_NUM_EXTENSIONS, &count);
	if (count < 0 || count > 65536)
		import_fail(name, "driver returned an invalid extension count");
	for (GLint index = 0; index < count; index++)
	{
		const GLubyte *extension = get_string_i(GL_EXTENSIONS, (GLuint)index);
		if (extension && strcmp((const char *)extension, wanted) == 0)
		{
			found = 1;
			break;
		}
	}
	cdecl_return32((uint32_t)found);
}

static void import_read_buffer_word(void)
{
	const char *name = "host_gl_read_buffer_word";
	GLuint buffer = argument(0, name);
	uint32_t offset = argument(1, name);
	GLint previous = 0;
	uint32_t value = 0;
	gl_get_integer_fn get_integer = (gl_get_integer_fn)mac_macos_gl_native_proc("glGetIntegerv");
	gl_get_buffer_sub_data_fn get_sub_data =
		(gl_get_buffer_sub_data_fn)mac_macos_gl_native_proc("glGetBufferSubData");
	typedef void (APIENTRYP bind_buffer_fn)(GLenum, GLuint);
	bind_buffer_fn bind_buffer = (bind_buffer_fn)mac_macos_gl_native_proc("glBindBuffer");
	if (!get_integer || !get_sub_data || !bind_buffer)
		import_fail(name, "buffer readback functions are unavailable");
	get_integer(GL_ARRAY_BUFFER_BINDING, &previous);
	bind_buffer(GL_ARRAY_BUFFER, buffer);
	get_sub_data(GL_ARRAY_BUFFER, (GLintptr)offset, sizeof(value), &value);
	bind_buffer(GL_ARRAY_BUFFER, (GLuint)previous);
	cdecl_return32(value);
}

static void import_buffer_write(void)
{
	const char *name = "host_gl_buffer_write";
	GLenum target = argument(0, name);
	uint32_t offset = argument(1, name);
	uint32_t size = argument(2, name);
	uint32_t data_va = argument(3, name);
	const void *data;
	gl_map_buffer_range_fn map_range;
	gl_unmap_buffer_fn unmap;
	gl_buffer_sub_data_fn sub_data;
	void *mapping;
	if (!size)
	{
		cdecl_return(name);
		return;
	}
	data = mac_guest_address_resolve(data_va, size);
	if (!data)
		import_fail(name, "source data span is unmapped");
	map_range = (gl_map_buffer_range_fn)mac_macos_gl_native_proc("glMapBufferRange");
	unmap = (gl_unmap_buffer_fn)mac_macos_gl_native_proc("glUnmapBuffer");
	sub_data = (gl_buffer_sub_data_fn)mac_macos_gl_native_proc("glBufferSubData");
	if (!map_range || !unmap || !sub_data)
		import_fail(name, "buffer update functions are unavailable");
	mapping = map_range(target, (GLintptr)offset, (GLsizeiptr)size,
		GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_RANGE_BIT);
	if (!mapping)
		sub_data(target, (GLintptr)offset, (GLsizeiptr)size, data);
	else
	{
		memcpy(mapping, data, size);
		if (!unmap(target))
			import_fail(name, "mapped buffer became corrupt during update");
	}
	cdecl_return(name);
}

static void import_fence_frame(void)
{
	static unsigned trace_calls;
	int trace = getenv("HALO_GL_FENCE_TRACE") != NULL;
	uint64_t begun_ns = trace ? SDL_GetTicksNS() : 0;
	const char *name = "host_gl_fence_frame";
	uint32_t slot = argument(0, name);
	gl_delete_sync_fn delete_sync;
	gl_fence_sync_fn fence_sync;
	if (slot >= FRAME_FENCE_SLOTS)
	{
		cdecl_return(name);
		return;
	}
	delete_sync = (gl_delete_sync_fn)mac_macos_gl_native_proc("glDeleteSync");
	fence_sync = (gl_fence_sync_fn)mac_macos_gl_native_proc("glFenceSync");
	if (!delete_sync || !fence_sync)
		import_fail(name, "GL sync functions are unavailable");
	if (frame_fences[slot])
		delete_sync(frame_fences[slot]);
	frame_fences[slot] = fence_sync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	if (trace && ++trace_calls <= 500)
		fprintf(stderr, "[gl-fence] call=%u slot=%u elapsed_ms=%.3f context=%p thread=%llu valid=%d\n",
			trace_calls, slot, (SDL_GetTicksNS() - begun_ns) / 1e6,
			(void *)SDL_GL_GetCurrentContext(), (unsigned long long)SDL_GetCurrentThreadID(),
			frame_fences[slot] != NULL);
	cdecl_return(name);
}

static void import_wait_frame(void)
{
	static unsigned trace_calls;
	int trace = getenv("HALO_GL_FENCE_TRACE") != NULL;
	uint64_t begun_ns = trace ? SDL_GetTicksNS() : 0;
	const char *name = "host_gl_wait_frame";
	uint32_t slot = argument(0, name);
	gl_client_wait_sync_fn wait_sync;
	gl_delete_sync_fn delete_sync;
	if (slot >= FRAME_FENCE_SLOTS || !frame_fences[slot])
	{
		cdecl_return(name);
		return;
	}
	wait_sync = (gl_client_wait_sync_fn)mac_macos_gl_native_proc("glClientWaitSync");
	delete_sync = (gl_delete_sync_fn)mac_macos_gl_native_proc("glDeleteSync");
	if (!wait_sync || !delete_sync)
		import_fail(name, "GL sync functions are unavailable");
	GLenum result = wait_sync(frame_fences[slot], GL_SYNC_FLUSH_COMMANDS_BIT, UINT64_C(1000000000));
	delete_sync(frame_fences[slot]);
	frame_fences[slot] = NULL;
	if (trace && ++trace_calls <= 500)
		fprintf(stderr, "[gl-fence-wait] call=%u slot=%u elapsed_ms=%.3f polls=1 status=0x%x failed=%d context=%p thread=%llu\n",
			trace_calls, slot, (SDL_GetTicksNS() - begun_ns) / 1e6, result,
			result == GL_WAIT_FAILED, (void *)SDL_GL_GetCurrentContext(),
			(unsigned long long)SDL_GetCurrentThreadID());
	if (result == GL_WAIT_FAILED)
		import_fail(name, "GPU frame fence wait failed");
	cdecl_return(name);
}

static void import_guest_gl_get_proc_address(void)
{
	const uint32_t name_va = argument(0, "guest_gl_get_proc_address");
	cdecl_return32(mac_macos_gl_token_for_guest_name(name_va));
}

typedef void (APIENTRYP gl_enable_fn)(GLenum);
typedef GLboolean (APIENTRYP gl_is_enabled_fn)(GLenum);
typedef void (APIENTRYP gl_gen_framebuffers_fn)(GLsizei, GLuint *);
typedef void (APIENTRYP gl_bind_framebuffer_fn)(GLenum, GLuint);
typedef void (APIENTRYP gl_framebuffer_texture_2d_fn)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef void (APIENTRYP gl_read_buffer_fn)(GLenum);
typedef void (APIENTRYP gl_draw_buffers_fn)(GLsizei, const GLenum *);
typedef GLenum (APIENTRYP gl_check_framebuffer_status_fn)(GLenum);
typedef void (APIENTRYP gl_blit_framebuffer_fn)(GLint, GLint, GLint, GLint,
	GLint, GLint, GLint, GLint, GLbitfield, GLenum);

static void native_draw_buffer(uint32_t buffer)
{
	gl_draw_buffers_fn draw_buffers = (gl_draw_buffers_fn)mac_macos_gl_native_proc("glDrawBuffers");
	GLenum target = (GLenum)buffer;
	if (!draw_buffers)
		import_fail("halo_gl41_copy_image_2d", "glDrawBuffers is unavailable");
	draw_buffers(1, &target);
}

static int make_native_copy_api(struct halo_gl41_copy_api *api)
{
	api->get_integer = (void (*)(uint32_t, int32_t *))mac_macos_gl_native_proc("glGetIntegerv");
	api->is_enabled = (uint8_t (*)(uint32_t))mac_macos_gl_native_proc("glIsEnabled");
	api->enable = (void (*)(uint32_t))mac_macos_gl_native_proc("glEnable");
	api->disable = (void (*)(uint32_t))mac_macos_gl_native_proc("glDisable");
	api->gen_framebuffers = (void (*)(int32_t, uint32_t *))mac_macos_gl_native_proc("glGenFramebuffers");
	api->bind_framebuffer = (void (*)(uint32_t, uint32_t))mac_macos_gl_native_proc("glBindFramebuffer");
	api->framebuffer_texture_2d = (void (*)(uint32_t, uint32_t, uint32_t, uint32_t, int32_t))
		mac_macos_gl_native_proc("glFramebufferTexture2D");
	api->read_buffer = (void (*)(uint32_t))mac_macos_gl_native_proc("glReadBuffer");
	api->draw_buffer = native_draw_buffer;
	api->check_framebuffer_status = (uint32_t (*)(uint32_t))
		mac_macos_gl_native_proc("glCheckFramebufferStatus");
	api->blit_framebuffer = (void (*)(int32_t, int32_t, int32_t, int32_t,
		int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t))
		mac_macos_gl_native_proc("glBlitFramebuffer");
	return api->get_integer && api->is_enabled && api->enable && api->disable &&
		api->gen_framebuffers && api->bind_framebuffer && api->framebuffer_texture_2d &&
		api->read_buffer && api->draw_buffer && api->check_framebuffer_status &&
		api->blit_framebuffer;
}

static void import_gl41_copy_image_2d(void)
{
	const char *name = "halo_gl41_copy_image_2d";
	uint32_t api_va = argument(0, name);
	uint32_t state_va = argument(1, name);
	struct halo_gl41_copy_api native_api;
	struct halo_gl41_copy_state state;
	int result;
	/* The guest API table contains synthetic 32-bit GL callable tokens. Check
	 * that the expected object exists, but never cast those tokens to pointers. */
	if (!api_va || !mac_guest_address_resolve(api_va, 11u * sizeof(uint32_t)))
		import_fail(name, "guest GL API descriptor is unmapped");
	if (!state_va || mac_guest_read(state_va, &state, sizeof(state)) != 0)
		import_fail(name, "guest copy-state object is unmapped");
	if (!make_native_copy_api(&native_api))
		import_fail(name, "required native OpenGL 4.1 entry point is unavailable");
	result = mac_gl41_copy_image_2d(&native_api, &state,
		argument(2, name), (int32_t)argument(3, name),
		argument(4, name), (int32_t)argument(5, name),
		(int32_t)argument(6, name), (int32_t)argument(7, name));
	if (mac_guest_write(state_va, &state, sizeof(state)) != 0)
		import_fail(name, "cannot write updated private FBO state to guest memory");
	cdecl_return32((uint32_t)result);
}

mac_guest_import_bridge mac_macos_gl_host_import_bridge(const char *name)
{
	static const struct
	{
		const char *name;
		mac_guest_import_bridge bridge;
	} imports[] = {
		{ "guest_gl_get_proc_address", import_guest_gl_get_proc_address },
		{ "halo_gl41_copy_image_2d", import_gl41_copy_image_2d },
		{ "host_gl_buffer_write", import_buffer_write },
		{ "host_gl_fence_frame", import_fence_frame },
		{ "host_gl_get_string", import_get_string },
		{ "host_gl_has_extension", import_has_extension },
		{ "host_gl_read_buffer_word", import_read_buffer_word },
		{ "host_gl_wait_frame", import_wait_frame },
	};
	for (size_t index = 0; index < sizeof(imports) / sizeof(imports[0]); index++)
	{
		if (name && strcmp(name, imports[index].name) == 0)
			return imports[index].bridge;
	}
	return NULL;
}
