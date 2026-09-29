#include "../gl41_compat.h"

#include <assert.h>
/* The generated guest alias has this exact public name. Its coexistence must
 * never redirect the native helper back into the guest import dispatcher. */
void halo_gl41_copy_image_2d(void)
{
	assert(!"native copy called the guest import alias");
}
#include <stdio.h>

struct mock_gl
{
	int32_t read_binding, draw_binding;
	int scissor;
	int complete;
	int blits;
	int32_t width, height, source_level, destination_level;
	uint32_t source, destination;
};

static struct mock_gl mock;

static void get_integer(uint32_t name, int32_t *value)
{
	if (name == 0x8CAA) *value = mock.read_binding;
	else if (name == 0x8CA6) *value = mock.draw_binding;
	else assert(0);
}

static uint8_t is_enabled(uint32_t capability)
{
	assert(capability == 0x0C11);
	return (uint8_t)mock.scissor;
}

static void enable(uint32_t capability)
{
	assert(capability == 0x0C11);
	mock.scissor = 1;
}

static void disable(uint32_t capability)
{
	assert(capability == 0x0C11);
	mock.scissor = 0;
}

static void gen_framebuffers(int32_t count, uint32_t *framebuffers)
{
	assert(count == 2);
	framebuffers[0] = 40;
	framebuffers[1] = 41;
}

static void bind_framebuffer(uint32_t target, uint32_t framebuffer)
{
	if (target == 0x8CA8) mock.read_binding = (int32_t)framebuffer;
	else if (target == 0x8CA9) mock.draw_binding = (int32_t)framebuffer;
	else assert(0);
}

static void framebuffer_texture_2d(uint32_t target, uint32_t attachment,
	uint32_t texture_target, uint32_t texture, int32_t level)
{
	assert(target == 0x8CA8 || target == 0x8CA9);
	assert(attachment == 0x8CE0 && texture_target == 0x0DE1);
	if (target == 0x8CA8)
	{
		mock.source = texture;
		mock.source_level = level;
	}
	else
	{
		mock.destination = texture;
		mock.destination_level = level;
	}
}

static void read_buffer(uint32_t source) { assert(source == 0x8CE0); }
static void draw_buffer(uint32_t destination) { assert(destination == 0x8CE0); }
static uint32_t check_framebuffer_status(uint32_t target)
{
	assert(target == 0x8CA8 || target == 0x8CA9);
	return mock.complete ? 0x8CD5 : 0;
}

static void blit_framebuffer(int32_t sx0, int32_t sy0, int32_t sx1, int32_t sy1,
	int32_t dx0, int32_t dy0, int32_t dx1, int32_t dy1, uint32_t mask, uint32_t filter)
{
	assert(sx0 == 0 && sy0 == 0 && dx0 == 0 && dy0 == 0);
	assert(sx1 == dx1 && sy1 == dy1);
	assert(mask == 0x4000 && filter == 0x2600);
	mock.width = sx1;
	mock.height = sy1;
	mock.blits++;
}

int main(void)
{
	const struct halo_gl41_copy_api api = {
		get_integer, is_enabled, enable, disable, gen_framebuffers,
		bind_framebuffer, framebuffer_texture_2d, read_buffer, draw_buffer,
		check_framebuffer_status, blit_framebuffer,
	};
	struct halo_gl41_copy_state state = { 0, 0 };

	mock = (struct mock_gl){ 3, 7, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
	assert(mac_gl41_copy_image_2d(&api, &state, 12, 2, 19, 3, 128, 64));
	assert(state.read_framebuffer == 40 && state.draw_framebuffer == 41);
	assert(mock.source == 12 && mock.source_level == 2);
	assert(mock.destination == 19 && mock.destination_level == 3);
	assert(mock.width == 128 && mock.height == 64 && mock.blits == 1);
	assert(mock.read_binding == 3 && mock.draw_binding == 7 && mock.scissor == 1);

	mock.complete = 0;
	mock.scissor = 0;
	assert(!mac_gl41_copy_image_2d(&api, &state, 12, 0, 19, 0, 32, 32));
	assert(mock.blits == 1);
	assert(mock.read_binding == 3 && mock.draw_binding == 7 && mock.scissor == 0);
	assert(!mac_gl41_copy_image_2d(&api, &state, 0, 0, 19, 0, 32, 32));

	puts("OpenGL 4.1 FBO-copy fallback: state-preservation cases passed");
	return 0;
}
