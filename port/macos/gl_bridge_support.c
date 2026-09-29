#include "gl_bridge_support.h"

#include "guest_address.h"
#include "guest_allocator.h"
#include "guest_call.h"
#include "gl_token_registry.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void *mac_macos_gl_native_proc(const char *name);
extern int mac_guest_allocate(size_t size, size_t alignment, uint32_t *guest_va_out);

static void gl_bridge_fatal(const char *name, const char *reason)
{
	fprintf(stderr, "[mac-gl-bridge] %s: %s\n", name ? name : "?",
		reason ? reason : "bridge failure");
	abort();
}

void mac_macos_gl_bridge_fault(const char *name, const char *reason)
{
	gl_bridge_fatal(name, reason);
}

static size_t component_count(uint32_t format)
{
	switch (format)
	{
	case GL_RED:
	case GL_GREEN:
	case GL_BLUE:
	case GL_ALPHA:
	case GL_LUMINANCE:
	case GL_DEPTH_COMPONENT:
	case GL_STENCIL_INDEX:
		return 1;
	case GL_RG:
		return 2;
	case GL_RGB:
	case GL_BGR:
		return 3;
	case GL_RGBA:
	case GL_BGRA:
		return 4;
	default:
		return 0;
	}
}

static size_t scalar_bytes(uint32_t type)
{
	switch (type)
	{
	case GL_BYTE:
	case GL_UNSIGNED_BYTE:
		return 1;
	case GL_SHORT:
	case GL_UNSIGNED_SHORT:
	case GL_HALF_FLOAT:
		return 2;
	case GL_INT:
	case GL_UNSIGNED_INT:
	case GL_FLOAT:
		return 4;
	case GL_DOUBLE:
		return 8;
	case GL_UNSIGNED_SHORT_5_6_5:
	case GL_UNSIGNED_SHORT_4_4_4_4:
	case GL_UNSIGNED_SHORT_5_5_5_1:
	case GL_UNSIGNED_INT_8_8_8_8:
	case GL_UNSIGNED_INT_10_10_10_2:
	case GL_INT_2_10_10_10_REV:
	case GL_UNSIGNED_INT_2_10_10_10_REV:
	case GL_UNSIGNED_INT_10F_11F_11F_REV:
	case GL_UNSIGNED_INT_5_9_9_9_REV:
	case GL_UNSIGNED_INT_24_8:
		return 0;
	case GL_FLOAT_32_UNSIGNED_INT_24_8_REV:
		return 8;
	default:
		return 0;
	}
}

size_t mac_macos_gl_pixel_span(uint32_t width, uint32_t height,
	uint32_t format, uint32_t type, uint32_t alignment)
{
	uint64_t row, stride, total;
	size_t components = component_count(format);
	size_t scalar = scalar_bytes(type);

	if (width == 0 || height == 0 || alignment == 0 ||
		(alignment & (alignment - 1)) != 0 || alignment > 8)
		return 0;
	if (!components || !scalar)
	{
		switch (type)
		{
		case GL_UNSIGNED_SHORT_5_6_5:
		case GL_UNSIGNED_SHORT_4_4_4_4:
		case GL_UNSIGNED_SHORT_5_5_5_1:
			if (components != 3 && components != 4) return 0;
			scalar = 2;
			components = 1;
			break;
		case GL_UNSIGNED_INT_8_8_8_8:
		case GL_UNSIGNED_INT_10_10_10_2:
		case GL_INT_2_10_10_10_REV:
		case GL_UNSIGNED_INT_2_10_10_10_REV:
			if (components != 4) return 0;
			scalar = 4;
			components = 1;
			break;
		case GL_UNSIGNED_INT_10F_11F_11F_REV:
		case GL_UNSIGNED_INT_5_9_9_9_REV:
			if (components != 3) return 0;
			scalar = 4;
			components = 1;
			break;
		case GL_UNSIGNED_INT_24_8:
			if (format != GL_DEPTH_STENCIL) return 0;
			scalar = 4;
			components = 1;
			break;
		case GL_FLOAT_32_UNSIGNED_INT_24_8_REV:
			if (format != GL_DEPTH_STENCIL) return 0;
			scalar = 8;
			components = 1;
			break;
		default:
			return 0;
		}
	}
	row = (uint64_t)width * components * scalar;
	stride = (row + alignment - 1) & ~((uint64_t)alignment - 1);
	if (height > 1 && stride > (UINT64_MAX - row) / (height - 1))
		return 0;
	total = stride * (height - 1) + row;
	if (total == 0 || total > SIZE_MAX)
		return 0;
	return (size_t)total;
}

static int get_integer(uint32_t pname, GLint *value);

static size_t pixel_transfer_span(uint32_t width, uint32_t height,
	uint32_t depth, uint32_t format, uint32_t type, int pack)
{
	GLint alignment = 4, row_length = 0, image_height = 0;
	GLint skip_pixels = 0, skip_rows = 0, skip_images = 0;
	uint64_t pixel_bytes, row_pixels, row_bytes, row_stride, image_rows;
	uint64_t image_stride, total;
	const GLenum alignment_name = pack ? GL_PACK_ALIGNMENT : GL_UNPACK_ALIGNMENT;
	const GLenum row_length_name = pack ? GL_PACK_ROW_LENGTH : GL_UNPACK_ROW_LENGTH;
	const GLenum image_height_name = pack ? GL_PACK_IMAGE_HEIGHT : GL_UNPACK_IMAGE_HEIGHT;
	const GLenum skip_pixels_name = pack ? GL_PACK_SKIP_PIXELS : GL_UNPACK_SKIP_PIXELS;
	const GLenum skip_rows_name = pack ? GL_PACK_SKIP_ROWS : GL_UNPACK_SKIP_ROWS;
	const GLenum skip_images_name = pack ? GL_PACK_SKIP_IMAGES : GL_UNPACK_SKIP_IMAGES;

	if (!width || !height || !depth || !get_integer(alignment_name, &alignment) ||
		!get_integer(row_length_name, &row_length) ||
		!get_integer(image_height_name, &image_height) ||
		!get_integer(skip_pixels_name, &skip_pixels) ||
		!get_integer(skip_rows_name, &skip_rows) ||
		!get_integer(skip_images_name, &skip_images) || alignment <= 0 ||
		row_length < 0 || image_height < 0 || skip_pixels < 0 || skip_rows < 0 || skip_images < 0)
		return 0;
	pixel_bytes = mac_macos_gl_pixel_span(1, 1, format, type, 1);
	if (!pixel_bytes)
		return 0;
	row_pixels = row_length > 0 ? (uint32_t)row_length : width;
	image_rows = image_height > 0 ? (uint32_t)image_height :
		(uint64_t)(uint32_t)skip_rows + height;
	if (row_pixels < (uint64_t)(uint32_t)skip_pixels + width ||
		(image_height > 0 && image_rows < (uint64_t)(uint32_t)skip_rows + height))
		return 0;
	row_bytes = row_pixels * pixel_bytes;
	if (row_bytes > UINT64_MAX - ((uint32_t)alignment - 1))
		return 0;
	row_stride = (row_bytes + (uint32_t)alignment - 1) & ~((uint64_t)alignment - 1);
	if (row_stride && image_rows > UINT64_MAX / row_stride)
		return 0;
	image_stride = row_stride * image_rows;
	uint64_t last_image = (uint64_t)depth - 1 + (uint32_t)skip_images;
	if (image_stride && last_image > (UINT64_MAX / image_stride))
		return 0;
	total = last_image * image_stride;
	if (row_stride && (uint64_t)(skip_rows + height - 1) > UINT64_MAX / row_stride)
		return 0;
	if (total > UINT64_MAX - (uint64_t)(skip_rows + height - 1) * row_stride)
		return 0;
	total += (uint64_t)(skip_rows + height - 1) * row_stride;
	if ((uint64_t)skip_pixels > UINT64_MAX / pixel_bytes)
		return 0;
	if (total > UINT64_MAX - (uint64_t)skip_pixels * pixel_bytes)
		return 0;
	total += (uint64_t)skip_pixels * pixel_bytes;
	if (total > UINT64_MAX - (uint64_t)width * pixel_bytes)
		return 0;
	total += (uint64_t)width * pixel_bytes;
	return total && total <= SIZE_MAX ? (size_t)total : 0;
}

static int get_integer(uint32_t pname, GLint *value)
{
	typedef void (APIENTRYP get_integer_fn)(GLenum, GLint *);
	get_integer_fn fn = (get_integer_fn)mac_macos_gl_native_proc("glGetIntegerv");
	if (!fn)
		return 0;
	fn((GLenum)pname, value);
	return 1;
}

static int pixel_buffer_is_bound(int pack)
{
	GLint binding = 0;
	return get_integer(pack ? GL_PIXEL_PACK_BUFFER_BINDING :
		GL_PIXEL_UNPACK_BUFFER_BINDING, &binding) && binding != 0;
}

static size_t texture_image_span(uint32_t target, uint32_t level,
	uint32_t format, uint32_t type, int pack)
{
	typedef void (APIENTRYP get_level_fn)(GLenum, GLint, GLenum, GLint *);
	get_level_fn get_level = (get_level_fn)mac_macos_gl_native_proc(
		"glGetTexLevelParameteriv");
	GLint width = 0, height = 1, depth = 1, pbo = 0;
	uint32_t binding = pack ? GL_PIXEL_PACK_BUFFER_BINDING : GL_PIXEL_UNPACK_BUFFER_BINDING;

	if (!get_level || !get_integer(binding, &pbo))
		return 0;
	get_level((GLenum)target, (GLint)level, GL_TEXTURE_WIDTH, &width);
	get_level((GLenum)target, (GLint)level, GL_TEXTURE_HEIGHT, &height);
	if ((GLenum)target == GL_TEXTURE_3D || (GLenum)target == GL_TEXTURE_2D_ARRAY)
		get_level((GLenum)target, (GLint)level, GL_TEXTURE_DEPTH, &depth);
	if (width <= 0 || height <= 0 || depth <= 0)
		return 0;
	return pixel_transfer_span((uint32_t)width, (uint32_t)height,
		(uint32_t)depth, format, type, pack);
}

static size_t gl_pointer_span(const char *name, unsigned index,
	const uint32_t *a, unsigned count)
{
	uint64_t n;
	if (!name || !a)
		return 0;
#define NAME_IS(value) (strcmp(name, value) == 0)
	if (NAME_IS("glGetIntegerv") && index == 1)
	{
		switch (a[0])
		{
		case GL_VIEWPORT:
		case GL_SCISSOR_BOX:
		case GL_COLOR_WRITEMASK: return 4 * sizeof(GLint);
		case GL_MAX_VIEWPORT_DIMS: return 2 * sizeof(GLint);
		default: return sizeof(GLint);
		}
	}
	if (NAME_IS("glGetTexImage") && index == 4 && count >= 5)
		return texture_image_span(a[0], a[1], a[2], a[3], 1);
	if (NAME_IS("glReadPixels") && index == 6 && count >= 7)
	{
		return pixel_transfer_span(a[2], a[3], 1, a[4], a[5], 1);
	}
	if ((NAME_IS("glGenTextures") || NAME_IS("glDeleteTextures") ||
		NAME_IS("glGenSamplers") || NAME_IS("glGenFramebuffers") ||
		NAME_IS("glDeleteFramebuffers") || NAME_IS("glGenBuffers") ||
		NAME_IS("glDeleteBuffers") || NAME_IS("glGenVertexArrays") ||
		NAME_IS("glGenQueries")) && count >= 2 && index == 1)
	{
		n = (uint64_t)a[0] * sizeof(GLuint);
		return n <= SIZE_MAX ? (size_t)n : 0;
	}
	if ((NAME_IS("glTexImage2D") && index == 8 && count >= 9) ||
		(NAME_IS("glTexSubImage2D") && index == 8 && count >= 9))
	{
		return pixel_transfer_span(a[3], a[4], 1, a[6], a[7], 0);
	}
	if (NAME_IS("glTexImage3D") && index == 9 && count >= 10)
	{
		return pixel_transfer_span(a[3], a[4], a[5], a[7], a[8], 0);
	}
	if (NAME_IS("glCompressedTexImage2D") && index == 7 && count >= 8) return a[6];
	if (NAME_IS("glCompressedTexImage3D") && index == 8 && count >= 9) return a[7];
	if ((NAME_IS("glTexParameteriv") || NAME_IS("glTexParameterfv") ||
		NAME_IS("glSamplerParameterfv")) && index == (NAME_IS("glSamplerParameterfv") ? 2u : 2u))
		return (a[1] == 0x1004u /* GL_TEXTURE_BORDER_COLOR */ ||
			a[1] == 0x8E46u /* GL_TEXTURE_SWIZZLE_RGBA */) ? 4 * sizeof(GLint) : sizeof(GLint);
	if (NAME_IS("glDrawBuffers") && index == 1 && count >= 2)
	{
		n = (uint64_t)a[0] * sizeof(GLenum);
		return n <= SIZE_MAX ? (size_t)n : 0;
	}
	if (NAME_IS("glBufferData") && index == 2 && count >= 4) return (size_t)a[1];
	if (NAME_IS("glBufferSubData") && index == 3 && count >= 4) return (size_t)a[2];
	if ((NAME_IS("glVertexAttribPointer") && index == 5) ||
		(NAME_IS("glVertexAttribIPointer") && index == 4))
	{
		GLint bound = 0;
		if (!get_integer(GL_ARRAY_BUFFER_BINDING, &bound)) return 0;
		return bound ? SIZE_MAX : 0; /* VBO byte offset, not a guest pointer. */
	}
	if (NAME_IS("glVertexAttrib4fv") && index == 1) return 4 * sizeof(GLfloat);
	if ((NAME_IS("glDrawElements") && index == 3) ||
		(NAME_IS("glDrawElementsBaseVertex") && index == 3))
	{
		GLint bound = 0;
		if (!get_integer(GL_ELEMENT_ARRAY_BUFFER_BINDING, &bound)) return 0;
		if (bound) return SIZE_MAX;
		size_t element = a[2] == GL_UNSIGNED_BYTE ? 1 :
			a[2] == GL_UNSIGNED_SHORT ? 2 : a[2] == GL_UNSIGNED_INT ? 4 : 0;
		n = (uint64_t)a[1] * element;
		return element && n <= SIZE_MAX ? (size_t)n : 0;
	}
	if (NAME_IS("glGetShaderiv") && index == 2) return sizeof(GLint);
	if (NAME_IS("glGetShaderInfoLog") && count >= 4)
	{
		if (index == 2) return sizeof(GLsizei);
		if (index == 3) return a[1];
	}
	if (NAME_IS("glGetProgramiv") && index == 2) return sizeof(GLint);
	if (NAME_IS("glGetProgramInfoLog") && count >= 4)
	{
		if (index == 2) return sizeof(GLsizei);
		if (index == 3) return a[1];
	}
	if (NAME_IS("glUniform1iv") && index == 2 && count >= 3)
	{
		n = (uint64_t)a[1] * sizeof(GLint);
		return n <= SIZE_MAX ? (size_t)n : 0;
	}
	if (NAME_IS("glUniform4fv") && index == 2 && count >= 3)
	{
		n = (uint64_t)a[1] * 4 * sizeof(GLfloat);
		return n <= SIZE_MAX ? (size_t)n : 0;
	}
	if (NAME_IS("glGetQueryObjectuiv") && index == 2) return sizeof(GLuint);
	if (NAME_IS("glShaderSource") && index == 3 && count >= 4)
	{
		n = (uint64_t)a[1] * sizeof(GLint);
		return a[3] && n <= SIZE_MAX ? (size_t)n : 0;
	}
#undef NAME_IS
	return 0;
}

static int is_string_arg(const char *name, unsigned index)
{
	return ((strcmp(name, "glBindAttribLocation") == 0 ||
		strcmp(name, "glBindFragDataLocation") == 0 ||
		strcmp(name, "glGetUniformLocation") == 0) &&
		((strcmp(name, "glGetUniformLocation") == 0 && index == 1) || index == 2));
}

struct guest_gl_string_slot { GLenum name; uint32_t guest_va; };
static struct guest_gl_string_slot gl_string_slots[8];
static pthread_mutex_t gl_string_lock = PTHREAD_MUTEX_INITIALIZER;

static const char *guest_string_bounded(uint32_t guest_va, size_t limit)
{
	for (size_t length = 0; length < limit; length++)
	{
		if (length > UINT32_MAX - guest_va)
			return NULL;
		const char *character = (const char *)mac_guest_address_resolve(guest_va + (uint32_t)length, 1);
		if (!character)
			return NULL;
		if (*character == '\0')
			return (const char *)mac_guest_address_resolve(guest_va, length + 1);
	}
	return NULL;
}

static const char *guest_string(uint32_t guest_va)
{
	return guest_string_bounded(guest_va, 4096);
}

uint32_t mac_macos_gl_token_for_guest_name(uint32_t guest_name_va)
{
	const char *name = guest_string(guest_name_va);
	if (!name)
		return 0;
	return mac_macos_gl_token_for_name(name);
}

static const GLchar **shader_source_strings(uint32_t guest_array_va,
	uint32_t count, uint32_t lengths_va)
{
	static _Thread_local const GLchar *strings[256];
	if (count > 256 || (count && !guest_array_va))
		return NULL;
	for (uint32_t index = 0; index < count; index++)
	{
		uint32_t guest_string_va = 0;
		uint64_t pointer_address = (uint64_t)guest_array_va + (uint64_t)index * sizeof(uint32_t);
		if (pointer_address > UINT32_MAX || mac_guest_read((uint32_t)pointer_address,
			&guest_string_va, sizeof(guest_string_va)))
			return NULL;
		if (lengths_va)
		{
			int32_t length = -1;
			uint64_t length_address = (uint64_t)lengths_va + (uint64_t)index * sizeof(uint32_t);
			if (length_address > UINT32_MAX || mac_guest_read((uint32_t)length_address, &length, sizeof(length)) ||
				length < 0)
				strings[index] = guest_string_bounded(guest_string_va, 1024 * 1024);
			else
				strings[index] = (const GLchar *)mac_guest_address_resolve(guest_string_va,
					(size_t)length);
		}
		else
			strings[index] = guest_string_bounded(guest_string_va, 1024 * 1024);
		if (!strings[index]) return NULL;
	}
	return strings;
}

void *mac_macos_gl_guest_pointer(const char *name, unsigned arg_index,
	uint32_t guest_va, const uint32_t *raw_args, unsigned arg_count)
{
	size_t bytes;
	if (!name || (!raw_args && arg_count))
		return NULL;
	if (strcmp(name, "glShaderSource") == 0 && arg_index == 2 && arg_count >= 4)
	{
		const GLchar **strings = shader_source_strings(guest_va, raw_args[1], raw_args[3]);
		if (!strings && raw_args[1])
		{
			fprintf(stderr, "[shader-source-failure] shader=%u count=%u vector=%08x lengths=%08x\n",
				raw_args[0], raw_args[1], guest_va, raw_args[3]);
			gl_bridge_fatal(name, "shader string vector is invalid or exceeds 256 entries");
		}
		return (void *)strings;
	}
	if (is_string_arg(name, arg_index))
		return (void *)guest_string(guest_va);
	if (((strcmp(name, "glReadPixels") == 0 && arg_index == 6) ||
		(strcmp(name, "glGetTexImage") == 0 && arg_index == 4)) &&
		pixel_buffer_is_bound(1))
		return (void *)(uintptr_t)guest_va;
	if (((strcmp(name, "glTexImage2D") == 0 && arg_index == 8) ||
		(strcmp(name, "glTexImage3D") == 0 && arg_index == 9) ||
		(strcmp(name, "glTexSubImage2D") == 0 && arg_index == 8) ||
		(strcmp(name, "glCompressedTexImage2D") == 0 && arg_index == 7) ||
		(strcmp(name, "glCompressedTexImage3D") == 0 && arg_index == 8)) &&
		pixel_buffer_is_bound(0))
		return (void *)(uintptr_t)guest_va;
	if ((strcmp(name, "glVertexAttribPointer") == 0 && arg_index == 5) ||
		(strcmp(name, "glVertexAttribIPointer") == 0 && arg_index == 4) ||
		((strcmp(name, "glDrawElements") == 0 ||
		strcmp(name, "glDrawElementsBaseVertex") == 0) && arg_index == 3))
	{
		bytes = gl_pointer_span(name, arg_index, raw_args, arg_count);
		if (bytes == SIZE_MAX)
			return (void *)(uintptr_t)guest_va;
	}
	else
		bytes = gl_pointer_span(name, arg_index, raw_args, arg_count);
	if (!bytes)
		return NULL;
	return mac_guest_address_resolve(guest_va, bytes);
}

uint32_t mac_macos_gl_return_guest_pointer(const char *name,
	const void *host_pointer, const uint32_t *raw_args, unsigned arg_count)
{
	uint32_t guest_va = 0;
	if (!host_pointer)
		return 0;
	if (strcmp(name, "glGetString") == 0)
	{
		struct guest_gl_string_slot *slot = NULL;
		uint32_t pname;
		if (!raw_args || arg_count < 1)
			gl_bridge_fatal(name, "missing GL string name argument");
		pname = raw_args[0];
		pthread_mutex_lock(&gl_string_lock);
		for (size_t index = 0; index < sizeof(gl_string_slots) / sizeof(gl_string_slots[0]); index++)
		{
			if (gl_string_slots[index].guest_va && gl_string_slots[index].name == pname)
			{
				slot = &gl_string_slots[index];
				break;
			}
			if (!gl_string_slots[index].guest_va && !slot)
				slot = &gl_string_slots[index];
		}
		if (!slot)
		{
			pthread_mutex_unlock(&gl_string_lock);
			gl_bridge_fatal(name, "guest GL string slots exhausted");
		}
		if (!slot->guest_va)
		{
			if (mac_guest_allocate(4096, 16, &guest_va) != 0 || !guest_va)
			{
				pthread_mutex_unlock(&gl_string_lock);
				gl_bridge_fatal(name, "guest scratch allocator could not allocate GL string storage");
			}
			slot->name = pname;
			slot->guest_va = guest_va;
		}
		guest_va = slot->guest_va;
		pthread_mutex_unlock(&gl_string_lock);
		const char *text = (const char *)host_pointer;
		const char *end = memchr(text, '\0', 4096);
		if (!end)
			gl_bridge_fatal(name, "native GL string exceeds 4095 bytes");
		size_t bytes = (size_t)(end - text) + 1;
		char *destination = (char *)mac_guest_address_resolve(guest_va, bytes);
		if (!destination)
			gl_bridge_fatal(name, "guest scratch string allocation is not mapped");
		memcpy(destination, text, bytes);
		return guest_va;
	}
	if (mac_guest_address_from_host(host_pointer, 1, &guest_va) != 0)
		gl_bridge_fatal(name, "native pointer result has no guest address mapping");
	return guest_va;
}
