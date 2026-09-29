#include "../gl_bridge_support.h"
#include "../gl_token_registry.h"
#include "../guest_address.h"

#include <SDL3/SDL_opengl.h>

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define GUEST_BASE 0x2000u
#define GUEST_SIZE 0x8000u

static uint8_t guest_memory[GUEST_SIZE];
static uint32_t next_guest_alloc = GUEST_BASE + 0x3000;
static GLint pack_row_length = 0;
static GLint pack_skip_pixels = 0;
static GLint pack_skip_rows = 0;
static GLint array_buffer_binding = 0;
static GLint element_buffer_binding = 0;

static void APIENTRY fake_get_integer(GLenum name, GLint *value)
{
	switch (name)
	{
	case GL_PACK_ALIGNMENT: *value = 4; break;
	case GL_UNPACK_ALIGNMENT: *value = 4; break;
	case GL_PACK_ROW_LENGTH: *value = pack_row_length; break;
	case GL_PACK_IMAGE_HEIGHT: *value = 0; break;
	case GL_PACK_SKIP_PIXELS: *value = pack_skip_pixels; break;
	case GL_PACK_SKIP_ROWS: *value = pack_skip_rows; break;
	case GL_PACK_SKIP_IMAGES: *value = 0; break;
	case GL_PIXEL_PACK_BUFFER_BINDING: *value = 0; break;
	case GL_PIXEL_UNPACK_BUFFER_BINDING: *value = 0; break;
	case GL_ARRAY_BUFFER_BINDING: *value = array_buffer_binding; break;
	case GL_ELEMENT_ARRAY_BUFFER_BINDING: *value = element_buffer_binding; break;
	default: *value = 0; break;
	}
}

void *mac_macos_gl_native_proc(const char *name)
{
	return strcmp(name, "glGetIntegerv") == 0 ? (void *)fake_get_integer : NULL;
}

uint32_t mac_macos_gl_token_for_name(const char *name)
{
	return name && strcmp(name, "glViewport") == 0 ? 0xFE100080u : 0;
}

void *mac_guest_address_resolve(uint32_t guest_address, size_t size)
{
	if (guest_address < GUEST_BASE || size > GUEST_SIZE ||
		(uint64_t)guest_address + size > (uint64_t)GUEST_BASE + GUEST_SIZE)
		return NULL;
	return guest_memory + (guest_address - GUEST_BASE);
}

int mac_guest_address_from_host(const void *host_address, size_t size,
	uint32_t *guest_address_out)
{
	uintptr_t base = (uintptr_t)guest_memory, pointer = (uintptr_t)host_address;
	if (!guest_address_out || pointer < base || size > GUEST_SIZE ||
		pointer - base > GUEST_SIZE - size)
		return -1;
	*guest_address_out = GUEST_BASE + (uint32_t)(pointer - base);
	return 0;
}

int mac_guest_read(uint32_t guest_address, void *destination, size_t size)
{
	void *source = mac_guest_address_resolve(guest_address, size);
	if (!source || !destination) return -1;
	memcpy(destination, source, size);
	return 0;
}

int mac_guest_allocate(size_t size, size_t alignment, uint32_t *guest_va_out)
{
	if (!size || !guest_va_out || !alignment || (alignment & (alignment - 1))) return -1;
	uint64_t aligned = ((uint64_t)next_guest_alloc + alignment - 1) & ~(uint64_t)(alignment - 1);
	if (aligned + size > (uint64_t)GUEST_BASE + GUEST_SIZE) return -1;
	*guest_va_out = (uint32_t)aligned;
	next_guest_alloc = (uint32_t)(aligned + size);
	return 0;
}

static void put_guest(uint32_t va, const void *value, size_t size)
{
	void *destination = mac_guest_address_resolve(va, size);
	assert(destination);
	memcpy(destination, value, size);
}

int main(void)
{
	assert(mac_macos_gl_pixel_span(10, 4, GL_RGBA, GL_UNSIGNED_BYTE, 4) == 160);
	assert(mac_macos_gl_pixel_span(3, 2, GL_RGB, GL_UNSIGNED_BYTE, 4) == 21);
	assert(mac_macos_gl_pixel_span(3, 2, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, 4) == 14);
	assert(mac_macos_gl_pixel_span(3, 2, GL_RGBA, GL_FLOAT, 8) == 96);
	assert(mac_macos_gl_pixel_span(0, 1, GL_RGBA, GL_UNSIGNED_BYTE, 4) == 0);
	assert(mac_macos_gl_pixel_span(1, 1, GL_BGRA, GL_UNSIGNED_BYTE, 3) == 0);
	assert(mac_macos_gl_pixel_span(UINT32_MAX, UINT32_MAX, GL_RGBA, GL_FLOAT, 8) == 0);

	const uint32_t buffer_data_args[] = { GL_ARRAY_BUFFER, 16, GUEST_BASE + 0x100, GL_STATIC_DRAW };
	assert(mac_macos_gl_guest_pointer("glBufferData", 2, buffer_data_args[2],
		buffer_data_args, 4) == guest_memory + 0x100);

	array_buffer_binding = 1;
	const uint32_t vertex_args[] = { 0, 3, GL_FLOAT, GL_FALSE, 24, 0x28 };
	assert(mac_macos_gl_guest_pointer("glVertexAttribPointer", 5, 0x28,
		vertex_args, 6) == (void *)(uintptr_t)0x28);
	array_buffer_binding = 0;
	element_buffer_binding = 1;
	const uint32_t draw_args[] = { GL_TRIANGLES, 12, GL_UNSIGNED_SHORT, 0x40 };
	assert(mac_macos_gl_guest_pointer("glDrawElements", 3, 0x40,
		draw_args, 4) == (void *)(uintptr_t)0x40);
	element_buffer_binding = 0;

	pack_row_length = 4;
	pack_skip_pixels = 1;
	pack_skip_rows = 1;
	const uint32_t read_args[] = { 0, 0, 3, 2, GL_RGB, GL_UNSIGNED_BYTE, GUEST_BASE };
	assert(mac_macos_gl_guest_pointer("glReadPixels", 6, GUEST_BASE,
		read_args, 7) == guest_memory);
	pack_row_length = pack_skip_pixels = pack_skip_rows = 0;

	const uint32_t first_string = GUEST_BASE + 0x300;
	const uint32_t second_string = GUEST_BASE + 0x340;
	const uint32_t strings_va = GUEST_BASE + 0x200;
	const uint32_t lengths_va = GUEST_BASE + 0x240;
	const uint32_t string_ptrs[] = { first_string, second_string };
	const int32_t string_lengths[] = { 3, -1 };
	put_guest(strings_va, string_ptrs, sizeof(string_ptrs));
	put_guest(lengths_va, string_lengths, sizeof(string_lengths));
	put_guest(first_string, "abcX", 4);
	put_guest(second_string, "def\0", 4);
	put_guest(GUEST_BASE + 0x380, "glViewport", sizeof("glViewport"));
	put_guest(GUEST_BASE + 0x3A0, "glNotAFunction", sizeof("glNotAFunction"));
	assert(mac_macos_gl_token_for_guest_name(GUEST_BASE + 0x380) == 0xFE100080u);
	assert(mac_macos_gl_token_for_guest_name(GUEST_BASE + 0x3A0) == 0);
	assert(mac_macos_gl_token_for_guest_name(GUEST_BASE + GUEST_SIZE) == 0);
	const uint32_t shader_args[] = { 7, 2, strings_va, lengths_va };
	const GLchar **native_strings = (const GLchar **)mac_macos_gl_guest_pointer(
		"glShaderSource", 2, strings_va, shader_args, 4);
	assert(native_strings && strcmp(native_strings[0], "abcX") == 0 &&
		strcmp(native_strings[1], "def") == 0);
	assert(mac_macos_gl_guest_pointer("glShaderSource", 3, lengths_va,
		shader_args, 4) == guest_memory + 0x240);

	const uint32_t version_args[] = { GL_VERSION };
	const uint32_t version_va = mac_macos_gl_return_guest_pointer("glGetString",
		"4.1 mock driver", version_args, 1);
	const uint32_t renderer_args[] = { GL_RENDERER };
	const uint32_t renderer_va = mac_macos_gl_return_guest_pointer("glGetString",
		"mock renderer", renderer_args, 1);
	assert(version_va && renderer_va && version_va != renderer_va);
	assert(strcmp((const char *)mac_guest_address_resolve(version_va, 15), "4.1 mock driver") == 0);
	assert(strcmp((const char *)mac_guest_address_resolve(renderer_va, 14), "mock renderer") == 0);
	return 0;
}
