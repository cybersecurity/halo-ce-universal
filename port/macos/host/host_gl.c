/*
HOST_GL.C

OpenGL ES for the guest. Its generated entry points (guest_gl.c) import
hostgl_<function>, resolved here to ANGLE's function (OpenGL ES over Metal,
build/macos/Halo/libGLESv2.dylib, which SDL also loads for the context:
host_main.c); the arguments already have host types by then. Only strings
need copying back.
*/

#include "host.h"

#include <GLES3/gl32.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

void *host_gles_library;

static void *library_symbol(const char *name)
{
	return host_gles_library ? dlsym(host_gles_library, name) : NULL;
}

/* the generated thunks' (tools/android_gl_stubs.py --host-thunks) */
void *host_gl_thunk(const char *name, void *(*resolve)(const char *));

/* a guest import: the function's thunk, which turns guest pointers into
host ones */
void *host_gl_resolve(const char *name)
{
	return host_gl_thunk(name, library_symbol);
}

/* the entry points this file calls itself, from the same library (the
host is not linked with it: its install name is relative) */
#define HOST_GL_FUNCTIONS(X) \
	X(PFNGLBINDBUFFERPROC, glBindBuffer) \
	X(PFNGLBUFFERSUBDATAPROC, glBufferSubData) \
	X(PFNGLCLIENTWAITSYNCPROC, glClientWaitSync) \
	X(PFNGLDELETESYNCPROC, glDeleteSync) \
	X(PFNGLFENCESYNCPROC, glFenceSync) \
	X(PFNGLGETINTEGERVPROC, glGetIntegerv) \
	X(PFNGLGETSTRINGPROC, glGetString) \
	X(PFNGLGETSTRINGIPROC, glGetStringi) \
	X(PFNGLMAPBUFFERRANGEPROC, glMapBufferRange) \
	X(PFNGLUNMAPBUFFERPROC, glUnmapBuffer)

#define HOST_GL_DECLARE(type, name) static type host_##name;
HOST_GL_FUNCTIONS(HOST_GL_DECLARE)

int host_gl_load(void)
{
#define HOST_GL_LOAD(type, name) host_##name = (type)library_symbol(#name); if (!host_##name) return 0;
	HOST_GL_FUNCTIONS(HOST_GL_LOAD)
	return 1;
}

#define glBindBuffer host_glBindBuffer
#define glBufferSubData host_glBufferSubData
#define glClientWaitSync host_glClientWaitSync
#define glDeleteSync host_glDeleteSync
#define glFenceSync host_glFenceSync
#define glGetIntegerv host_glGetIntegerv
#define glGetString host_glGetString
#define glGetStringi host_glGetStringi
#define glMapBufferRange host_glMapBufferRange
#define glUnmapBuffer host_glUnmapBuffer

void host_gl_get_string(uint32_t name, int index, char *buffer, uint32_t size)
{
	const GLubyte *text = index >= 0 ? glGetStringi(name, (GLuint)index) : glGetString(name);

	if (!size)
		return;
	buffer[0] = 0;
	if (text)
	{
		strncpy(buffer, (const char *)text, size - 1);
		buffer[size - 1] = 0;
	}
}

int host_gl_has_extension(const char *name)
{
	GLint count = 0, index;

	glGetIntegerv(GL_NUM_EXTENSIONS, &count);
	for (index = 0; index < count; index++)
	{
		const char *extension = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)index);

		if (extension && !strcmp(extension, name))
			return 1;
	}
	return 0;
}

/* one 32-bit word of a buffer object (the visibility test counters of
d3d8_gl.c); ES has no glGetBufferSubData, and the mapping it offers
instead is a host pointer */
uint32_t host_gl_read_buffer_word(uint32_t buffer, uint32_t offset)
{
	uint32_t value = 0;
	GLint previous = 0;
	const void *mapping;

	glGetIntegerv(GL_ATOMIC_COUNTER_BUFFER_BINDING, &previous);
	glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, buffer);
	mapping = glMapBufferRange(GL_ATOMIC_COUNTER_BUFFER, offset, sizeof(value), GL_MAP_READ_BIT);
	if (mapping)
	{
		memcpy(&value, mapping, sizeof(value));
		glUnmapBuffer(GL_ATOMIC_COUNTER_BUFFER);
	}
	glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, (GLuint)previous);
	return value;
}

/* The renderer streams each frame's vertices and indices into the next of
a ring of buffers (d3d8_gl.c). A fence marks the end of each frame's work,
and a buffer is written again only once the GPU has passed the fence of the
frame that last used it: drivers queue several frames, and a draw still
waiting to run would otherwise read a later frame's vertices. */
#define FRAME_FENCE_SLOTS 8

static GLsync frame_fences[FRAME_FENCE_SLOTS];

void host_gl_fence_frame(uint32_t slot)
{
	if (slot >= FRAME_FENCE_SLOTS)
		return;
	if (frame_fences[slot])
		glDeleteSync(frame_fences[slot]);
	frame_fences[slot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

void host_gl_wait_frame(uint32_t slot)
{
	if (slot >= FRAME_FENCE_SLOTS || !frame_fences[slot])
		return;
	/* at most a second: a lost context must not hang the game */
	glClientWaitSync(frame_fences[slot], GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
	glDeleteSync(frame_fences[slot]);
	frame_fences[slot] = NULL;
}

/* writes data into the buffer bound to target. The renderer streams a
range per draw, so a frame makes hundreds of these, and the cost per call
rather than per byte is what a frame is made of.

GL_MAP_INVALIDATE_RANGE_BIT was what made that cost ruinous. It tells the
driver the range's previous contents are undefined and must be discarded,
which is the very work GL_MAP_UNSYNCHRONIZED_BIT exists to avoid: that one
promises the caller that no queued draw is reading the range. Asked to do
both, Adreno pays for the discard - about 0.85 ms a call on a Galaxy Z
Flip 4, whatever the range written - and with a few hundred calls a frame
that came to 96% of a 550 ms frame at 1.8 fps, with the GPU idle throughout.

Without the invalidation the same call is well under a microsecond and the
same game runs at the display's refresh rate. The promise unsynchronized
makes still holds: the renderer only writes ranges that no queued draw reads,
because host_gl_wait_frame releases the ring slot first. */
void host_gl_buffer_write(uint32_t target, uint32_t offset, uint32_t size, const void *data)
{
	void *mapping = glMapBufferRange(target, offset, size,
		GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);

	if (!mapping)
	{
		glBufferSubData(target, offset, size, data);
		return;
	}
	memcpy(mapping, data, size);
	glUnmapBuffer(target);
}
