#include "guest_address.h"
#include "guest_allocator.h"
#include "guest_call.h"
#include "guest_thread.h"
#include "host_services.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#define TEST_GUEST_BASE UINT32_C(0x50000000)
#define TEST_GUEST_SIZE (64u * 1024u)
#define TEST_THREAD_VA UINT32_C(0x50000100)
#define TEST_START_VA UINT32_C(0x12345678)
#define TEST_RETURN_VA UINT32_C(0xffffffff)

struct test_state
{
	pthread_mutex_t mutex;
	pthread_cond_t condition;
	uint32_t next;
	uint32_t initialized_esp;
	uint32_t initialized_thread;
	int dispatched;
	int released;
};

static struct test_state state = {
	PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, TEST_GUEST_BASE, 0, 0, 0, 0
};

static int test_allocate(void *context, size_t size, size_t alignment,
	uint32_t *guest_va_out)
{
	struct test_state *test = context;
	uint64_t aligned = ((uint64_t)test->next + alignment - 1) & ~(uint64_t)(alignment - 1);
	if (aligned + size > (uint64_t)TEST_GUEST_BASE + TEST_GUEST_SIZE)
		return ENOMEM;
	*guest_va_out = (uint32_t)aligned;
	test->next = (uint32_t)(aligned + size);
	return 0;
}

static void test_release(void *context, uint32_t guest_va, size_t size)
{
	struct test_state *test = context;
	assert(guest_va >= TEST_GUEST_BASE);
	assert(size >= 16 * 1024);
	pthread_mutex_lock(&test->mutex);
	test->released++;
	pthread_cond_broadcast(&test->condition);
	pthread_mutex_unlock(&test->mutex);
}

static void initialize_guest_thread(void *context, uint32_t thread_va,
	uint32_t guest_esp)
{
	struct test_state *test = context;
	assert(mac_host_get_guest_tp() == thread_va);
	pthread_mutex_lock(&test->mutex);
	test->initialized_thread = thread_va;
	test->initialized_esp = guest_esp;
	pthread_mutex_unlock(&test->mutex);
}

static void dispatch_guest(void *context, uint32_t function_va)
{
	struct test_state *test = context;
	uint32_t frame[2];
	uint32_t guest_esp;

	assert(function_va == TEST_START_VA);
	pthread_mutex_lock(&test->mutex);
	guest_esp = test->initialized_esp;
	pthread_mutex_unlock(&test->mutex);
	assert(mac_guest_read(guest_esp, frame, sizeof(frame)) == 0);
	assert(frame[0] == TEST_RETURN_VA);
	assert(frame[1] == TEST_THREAD_VA);
	assert(mac_host_get_guest_tp() == TEST_THREAD_VA);
	pthread_mutex_lock(&test->mutex);
	test->dispatched++;
	pthread_cond_broadcast(&test->condition);
	pthread_mutex_unlock(&test->mutex);
}

int main(void)
{
	void *memory = mmap(NULL, TEST_GUEST_SIZE, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	struct mac_guest_allocator allocator = { &state, test_allocate, test_release };
	struct mac_guest_thread_runtime runtime = { &state, initialize_guest_thread, dispatch_guest };
	struct timespec deadline;

	assert(memory != MAP_FAILED);
	assert(mac_guest_address_register(TEST_GUEST_BASE, memory, TEST_GUEST_SIZE) == 0);
	assert(mac_guest_allocator_install(&allocator) == 0);
	assert(mac_guest_thread_runtime_install(&runtime, TEST_START_VA) == 0);
	assert(mac_host_thread_create(TEST_THREAD_VA, 32 * 1024) == 0);

	assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
	deadline.tv_sec += 3;
	pthread_mutex_lock(&state.mutex);
	while (!state.released)
		assert(pthread_cond_timedwait(&state.condition, &state.mutex, &deadline) == 0);
	assert(state.dispatched == 1);
	assert(state.initialized_thread == TEST_THREAD_VA);
	assert(state.initialized_esp % 16 == 8);
	pthread_mutex_unlock(&state.mutex);
	assert(mac_host_get_guest_tp() == 0);

	mac_guest_address_reset();
	puts("guest_thread_test: ok");
	return 0;
}
