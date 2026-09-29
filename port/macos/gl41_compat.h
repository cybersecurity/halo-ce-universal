/*
GL41_COMPAT.H

Small compatibility helpers for the macOS OpenGL 4.1 renderer. The function
table is supplied by the renderer so this file does not depend on a GL loader
or on a particular SDL context owner.
*/

#ifndef HALO_MACOS_GL41_COMPAT_H
#define HALO_MACOS_GL41_COMPAT_H

#include <stdint.h>

struct halo_gl41_copy_api
{
	void (*get_integer)(uint32_t name, int32_t *value);
	uint8_t (*is_enabled)(uint32_t capability);
	void (*enable)(uint32_t capability);
	void (*disable)(uint32_t capability);
	void (*gen_framebuffers)(int32_t count, uint32_t *framebuffers);
	void (*bind_framebuffer)(uint32_t target, uint32_t framebuffer);
	void (*framebuffer_texture_2d)(uint32_t target, uint32_t attachment,
		uint32_t texture_target, uint32_t texture, int32_t level);
	void (*read_buffer)(uint32_t source);
	void (*draw_buffer)(uint32_t destination);
	uint32_t (*check_framebuffer_status)(uint32_t target);
	void (*blit_framebuffer)(int32_t src_x0, int32_t src_y0, int32_t src_x1, int32_t src_y1,
		int32_t dst_x0, int32_t dst_y0, int32_t dst_x1, int32_t dst_y1,
		uint32_t mask, uint32_t filter);
};

struct halo_gl41_copy_state
{
	uint32_t read_framebuffer;
	uint32_t draw_framebuffer;
};

/* Copies one 2D mip using a framebuffer blit. It saves and restores the
caller’s independent READ/DRAW framebuffer bindings and scissor enable state.
The two private FBOs are created lazily and belong to this state object. */
int mac_gl41_copy_image_2d(const struct halo_gl41_copy_api *api,
	struct halo_gl41_copy_state *state,
	uint32_t source, int32_t source_level,
	uint32_t destination, int32_t destination_level,
	int32_t width, int32_t height);

#endif
