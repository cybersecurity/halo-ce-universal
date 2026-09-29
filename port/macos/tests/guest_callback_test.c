#include "guest_address.h"
#include "guest_allocator.h"
#include "guest_callback.h"
#include "guest_call.h"
#include "host_services.h"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>

#define TEST_GUEST_BASE UINT32_C(0x12000000)
#define TEST_GUEST_SIZE (1024u * 1024u)
#define TEST_RETURN UINT32_C(0xffffffff)
#define TEST_ATTACH_VA UINT32_C(0x00401000)
#define TEST_CALLBACK_VA UINT32_C(0x00402000)
#define TEST_TP UINT32_C(0x12001000)

static unsigned char guest_memory[TEST_GUEST_SIZE];
static uint32_t allocation_cursor = TEST_GUEST_BASE;
static uint32_t current_esp;
static uint32_t current_function;
static uint32_t observed_args[4];
static uint32_t observed_return;
static uint32_t observed_thread;

static int test_allocate(void *context, size_t size, size_t alignment,
	uint32_t *guest_va_out)
{
	uint64_t aligned = ((uint64_t)allocation_cursor + alignment - 1) & ~(uint64_t)(alignment - 1);
	(void)context;
	if (!guest_va_out || aligned + size > TEST_GUEST_BASE + TEST_GUEST_SIZE)
		return -1;
	*guest_va_out = (uint32_t)aligned;
	allocation_cursor = (uint32_t)(aligned + size);
	return 0;
}

static void test_release(void *context, uint32_t guest_va, size_t size)
{
	(void)context;
	(void)guest_va;
	(void)size;
}

static void test_initialize(void *context, uint32_t guest_thread_va, uint32_t guest_esp)
{
	(void)context;
	observed_thread = guest_thread_va;
	current_esp = guest_esp;
}

static void test_dispatch(void *context, uint32_t guest_function_va)
{
	uint32_t frame[5] = {0};
	(void)context;
	current_function = guest_function_va;
	if (guest_function_va == TEST_ATTACH_VA)
	{
		mac_host_set_guest_tp(TEST_TP);
		return;
	}
	assert(guest_function_va == TEST_CALLBACK_VA);
	assert(mac_guest_read(current_esp, frame, sizeof(frame)) == 0);
	observed_return = frame[0];
	memcpy(observed_args, frame + 1, sizeof(observed_args));
}

int main(void)
{
	struct mac_guest_allocator allocator = { NULL, test_allocate, test_release };
	struct mac_guest_thread_runtime runtime = { NULL, test_initialize, test_dispatch };
	struct mac_guest_callback_context *callback = NULL;
	const uint32_t args[] = { 0x11, 0x22, 0x33, 0x44 };

	assert(mac_guest_address_register(TEST_GUEST_BASE, guest_memory, sizeof(guest_memory)) == 0);
	assert(mac_guest_allocator_install(&allocator) == 0);
	assert(mac_guest_callback_runtime_install(&runtime, TEST_ATTACH_VA) == 0);
	assert(mac_guest_callback_context_create_current(64u * 1024u, &callback) == 0);
	assert(callback != NULL);
	assert(mac_host_get_guest_tp() == TEST_TP);
	assert(current_function == TEST_ATTACH_VA);
	assert(observed_thread == 0);

	assert(mac_guest_callback_invoke(callback, TEST_CALLBACK_VA, args, 4) == 0);
	assert(current_function == TEST_CALLBACK_VA);
	assert(observed_thread == TEST_TP);
	assert(observed_return == TEST_RETURN);
	assert(memcmp(observed_args, args, sizeof(args)) == 0);

	mac_guest_callback_context_destroy_current(callback);
	assert(mac_host_get_guest_tp() == 0);
	mac_guest_address_reset();
	return 0;
}
