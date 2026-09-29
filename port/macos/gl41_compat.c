/* OpenGL 4.1 fallbacks used by the native macOS source port. */

#include "gl41_compat.h"

enum
{
	GL_READ_FRAMEBUFFER_VALUE = 0x8CA8,
	GL_DRAW_FRAMEBUFFER_VALUE = 0x8CA9,
	GL_READ_FRAMEBUFFER_BINDING_VALUE = 0x8CAA,
	GL_DRAW_FRAMEBUFFER_BINDING_VALUE = 0x8CA6,
	GL_SCISSOR_TEST_VALUE = 0x0C11,
	GL_COLOR_ATTACHMENT0_VALUE = 0x8CE0,
	GL_TEXTURE_2D_VALUE = 0x0DE1,
	GL_COLOR_BUFFER_BIT_VALUE = 0x00004000,
	GL_NEAREST_VALUE = 0x2600,
	GL_FRAMEBUFFER_COMPLETE_VALUE = 0x8CD5,
};

static int api_valid(const struct halo_gl41_copy_api *api)
{
	return api && api->get_integer && api->is_enabled && api->enable && api->disable &&
		api->gen_framebuffers && api->bind_framebuffer && api->framebuffer_texture_2d &&
		api->read_buffer && api->draw_buffer && api->check_framebuffer_status && api->blit_framebuffer;
}

int mac_gl41_copy_image_2d(const struct halo_gl41_copy_api *api,
	struct halo_gl41_copy_state *state,
	uint32_t source, int32_t source_level,
	uint32_t destination, int32_t destination_level,
	int32_t width, int32_t height)
{
	int32_t previous_read, previous_draw;
	int scissor_enabled;
	int result = 0;

	if (!api_valid(api) || !state || !source || !destination ||
		source_level < 0 || destination_level < 0 || width <= 0 || height <= 0)
		return 0;
	if (!state->read_framebuffer || !state->draw_framebuffer)
	{
		uint32_t framebuffers[2] = { 0, 0 };

		api->gen_framebuffers(2, framebuffers);
		if (!framebuffers[0] || !framebuffers[1] || framebuffers[0] == framebuffers[1])
			return 0;
		state->read_framebuffer = framebuffers[0];
		state->draw_framebuffer = framebuffers[1];
	}

	api->get_integer(GL_READ_FRAMEBUFFER_BINDING_VALUE, &previous_read);
	api->get_integer(GL_DRAW_FRAMEBUFFER_BINDING_VALUE, &previous_draw);
	scissor_enabled = api->is_enabled(GL_SCISSOR_TEST_VALUE) != 0;

	api->bind_framebuffer(GL_READ_FRAMEBUFFER_VALUE, state->read_framebuffer);
	api->framebuffer_texture_2d(GL_READ_FRAMEBUFFER_VALUE, GL_COLOR_ATTACHMENT0_VALUE,
		GL_TEXTURE_2D_VALUE, source, source_level);
	api->read_buffer(GL_COLOR_ATTACHMENT0_VALUE);
	if (api->check_framebuffer_status(GL_READ_FRAMEBUFFER_VALUE) != GL_FRAMEBUFFER_COMPLETE_VALUE)
		goto restore;

	api->bind_framebuffer(GL_DRAW_FRAMEBUFFER_VALUE, state->draw_framebuffer);
	api->framebuffer_texture_2d(GL_DRAW_FRAMEBUFFER_VALUE, GL_COLOR_ATTACHMENT0_VALUE,
		GL_TEXTURE_2D_VALUE, destination, destination_level);
	api->draw_buffer(GL_COLOR_ATTACHMENT0_VALUE);
	if (api->check_framebuffer_status(GL_DRAW_FRAMEBUFFER_VALUE) != GL_FRAMEBUFFER_COMPLETE_VALUE)
		goto restore;

	api->disable(GL_SCISSOR_TEST_VALUE);
	api->blit_framebuffer(0, 0, width, height, 0, 0, width, height,
		GL_COLOR_BUFFER_BIT_VALUE, GL_NEAREST_VALUE);
	result = 1;

restore:
	api->bind_framebuffer(GL_READ_FRAMEBUFFER_VALUE, (uint32_t)previous_read);
	api->bind_framebuffer(GL_DRAW_FRAMEBUFFER_VALUE, (uint32_t)previous_draw);
	if (scissor_enabled)
		api->enable(GL_SCISSOR_TEST_VALUE);
	else
		api->disable(GL_SCISSOR_TEST_VALUE);
	return result;
}
