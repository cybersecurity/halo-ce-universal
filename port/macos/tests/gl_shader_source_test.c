#include "gl_bridge_support.h"
#include "guest_address.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

static unsigned char memory[32768];

void *mac_macos_gl_native_proc(const char *name)
{
	(void)name;
	return NULL;
}

uint32_t mac_macos_gl_token_for_name(const char *name)
{
	(void)name;
	return 0;
}

int main(void)
{
	const uint32_t base = 0x10000;
	const uint32_t vector_va = base + 64;
	const uint32_t lengths_va = base + 96;
	const uint32_t source_va = base + 256;
	uint32_t raw[4] = {1, 1, vector_va, 0};
	const char *const *strings;
	assert(mac_guest_address_register(base, memory, sizeof(memory)) == 0);
	memcpy(memory + 64, &source_va, 4);
	memset(memory + 256, 'x', 9164);
	memory[256 + 9164] = 0;
	strings = mac_macos_gl_guest_pointer("glShaderSource", 2, vector_va, raw, 4);
	assert(strings && strings[0] == (const char *)memory + 256);
	assert(strlen(strings[0]) == 9164);
	/* Negative lengths retain GL's NUL-terminated source semantics. */
	int32_t length = -1;
	memcpy(memory + 96, &length, 4);
	raw[3] = lengths_va;
	strings = mac_macos_gl_guest_pointer("glShaderSource", 2, vector_va, raw, 4);
	assert(strings && strlen(strings[0]) == 9164);
	/* Explicit lengths permit a source with no terminating NUL. */
	length = 10000;
	memcpy(memory + 96, &length, 4);
	memset(memory + 256, 'y', (size_t)length);
	strings = mac_macos_gl_guest_pointer("glShaderSource", 2, vector_va, raw, 4);
	assert(strings && strings[0] == (const char *)memory + 256);
	assert(strings[0][9999] == 'y');
	assert(mac_guest_address_unregister(base) == 0);
	return 0;
}
