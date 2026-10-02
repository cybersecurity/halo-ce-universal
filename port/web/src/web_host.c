/*
WEB_HOST.C

The services the Android host supplies to the shared OpenGL ES renderer
(port/linux/src/xgpu.h), for WebGL 2.

The worker recorder snapshots buffer data when it is given, then merges
append-only stream writes before replaying their draws. A frame's commands
are flushed before presentation. The GPU queue is drained once per stream
ring rotation so drivers do not retain an unlimited number of upload copies.
WebGL fences cannot become signaled until the worker yields to its event loop;
this blocking game loop uses a periodic finish instead.
*/

#include <GLES3/gl3.h>
#include <string.h>

#include "platform.h"

int host_gl_has_extension(const char *name)
{
	/* WebGL's names for the extensions the renderer asks for */
	static const struct
	{
		const char *gl;
		const char *webgl;
	} aliases[] =
	{
		{ "GL_EXT_texture_compression_s3tc", "GL_WEBGL_compressed_texture_s3tc" },
		{ "GL_EXT_texture_compression_dxt1", "GL_WEBGL_compressed_texture_s3tc" },
		{ "GL_ANGLE_texture_compression_dxt3", "GL_WEBGL_compressed_texture_s3tc" },
		{ "GL_ANGLE_texture_compression_dxt5", "GL_WEBGL_compressed_texture_s3tc" },
		{ "GL_EXT_texture_filter_anisotropic", "GL_EXT_texture_filter_anisotropic" },
	};
	const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
	const char *wanted = name;
	size_t index, length;
	const char *found;

	if (!extensions || !name)
		return 0;
	for (index = 0; index < sizeof(aliases) / sizeof(aliases[0]); index++)
	{
		if (!strcmp(aliases[index].gl, name))
		{
			wanted = aliases[index].webgl;
			break;
		}
	}
	length = strlen(wanted);
	for (found = strstr(extensions, wanted); found; found = strstr(found + 1, wanted))
	{
		if ((found == extensions || found[-1] == ' ') && (found[length] == ' ' || found[length] == '\0'))
			return 1;
	}
	return 0;
}

unsigned int host_gl_read_buffer_word(unsigned int buffer, unsigned int offset)
{
	/* only for atomic counters, which WebGL 2 does not have */
	(void)buffer;
	(void)offset;
	return 0;
}

void host_gl_buffer_write(unsigned int target, unsigned int offset, unsigned int size, const void *data)
{
	glBufferSubData((GLenum)target, (GLintptr)offset, (GLsizeiptr)size, data);
}

void host_gl_fence_frame(unsigned int slot)
{
	(void)slot;
}

void host_gl_wait_frame(unsigned int slot)
{
	/* Three ring slots (d3d8_gl.c): let frames overlap, then complete their
	work before starting another rotation. Waiting happens on the game worker,
	not the page/network thread. glFinish is also a recorder replay barrier. */
	if (slot == 0)
		glFinish();
}
