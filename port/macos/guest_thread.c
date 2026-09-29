#include "guest_thread.h"

#include "guest_allocator.h"
#include "guest_call.h"
#include "host_services.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>

#define GUEST_THREAD_RETURN_SENTINEL UINT32_C(0xffffffff)
#define GUEST_STACK_ALIGNMENT 16u

struct thread_start
{
	struct mac_guest_thread_runtime runtime;
	uint32_t guest_thread_va;
	uint32_t guest_start_va;
	uint32_t guest_stack_base;
	size_t guest_stack_size;
	uint32_t initial_guest_esp;
};

static struct mac_guest_thread_runtime installed_runtime;
static uint32_t installed_guest_start_va;
static int runtime_installed;
static pthread_mutex_t runtime_mutex = PTHREAD_MUTEX_INITIALIZER;

int mac_guest_thread_runtime_install(const struct mac_guest_thread_runtime *runtime,
	uint32_t guest_start_va)
{
	int result = -1;

	if (!runtime || !runtime->initialize || !runtime->dispatch || !guest_start_va)
		return -1;
	pthread_mutex_lock(&runtime_mutex);
	if (!runtime_installed)
	{
		installed_runtime = *runtime;
		installed_guest_start_va = guest_start_va;
		runtime_installed = 1;
		result = 0;
	}
	pthread_mutex_unlock(&runtime_mutex);
	return result;
}

int mac_host_thread_create(uint32_t guest_thread_va, uint32_t stack_size)
{
	struct mac_guest_thread_runtime runtime;
	uint32_t guest_start_va;

	pthread_mutex_lock(&runtime_mutex);
	if (!runtime_installed)
	{
		pthread_mutex_unlock(&runtime_mutex);
		return EINVAL;
	}
	runtime = installed_runtime;
	guest_start_va = installed_guest_start_va;
	pthread_mutex_unlock(&runtime_mutex);
	return mac_guest_thread_create(&runtime, guest_thread_va, guest_start_va, stack_size);
}

static void *guest_thread_main(void *opaque)
{
	struct thread_start *start = opaque;
	struct mac_guest_thread_runtime runtime = start->runtime;
	uint32_t guest_thread_va = start->guest_thread_va;
	uint32_t guest_start_va = start->guest_start_va;
	uint32_t guest_stack_base = start->guest_stack_base;
	size_t guest_stack_size = start->guest_stack_size;
	uint32_t guest_esp = start->initial_guest_esp;

	free(start);
	mac_host_set_guest_tp(guest_thread_va);
	runtime.initialize(runtime.context, guest_thread_va, guest_esp);
	runtime.dispatch(runtime.context, guest_start_va);
	mac_host_set_guest_tp(0);
	(void)mac_guest_release(guest_stack_base, guest_stack_size);
	return NULL;
}

int mac_guest_thread_create(const struct mac_guest_thread_runtime *runtime,
	uint32_t guest_thread_va, uint32_t guest_start_va, size_t stack_size)
{
	struct thread_start *start;
	pthread_attr_t attributes;
	pthread_t thread;
	uint32_t guest_stack_base;
	size_t guest_stack_size;
	uint64_t guest_stack_end;
	uint32_t guest_stack_top, guest_esp;
	uint32_t frame[2];
	int error;

	if (!runtime || !runtime->initialize || !runtime->dispatch || !guest_thread_va ||
		!guest_start_va || stack_size < 16 * 1024)
		return EINVAL;
	guest_stack_size = stack_size;
	if (mac_guest_allocate(guest_stack_size, GUEST_STACK_ALIGNMENT, &guest_stack_base) != 0)
		return EAGAIN;
	guest_stack_end = (uint64_t)guest_stack_base + guest_stack_size;
	if (guest_stack_end > UINT32_MAX ||
		guest_stack_size < sizeof(frame) + GUEST_STACK_ALIGNMENT)
	{
		(void)mac_guest_release(guest_stack_base, guest_stack_size);
		return EINVAL;
	}
	guest_stack_top = (uint32_t)guest_stack_end & ~(GUEST_STACK_ALIGNMENT - 1);
	guest_esp = guest_stack_top - (uint32_t)sizeof(frame);
	frame[0] = GUEST_THREAD_RETURN_SENTINEL;
	frame[1] = guest_thread_va;
	if (mac_guest_write(guest_esp, frame, sizeof(frame)) != 0)
	{
		(void)mac_guest_release(guest_stack_base, guest_stack_size);
		return EFAULT;
	}

	start = malloc(sizeof(*start));
	if (!start)
	{
		(void)mac_guest_release(guest_stack_base, guest_stack_size);
		return ENOMEM;
	}
	start->runtime = *runtime;
	start->guest_thread_va = guest_thread_va;
	start->guest_start_va = guest_start_va;
	start->guest_stack_base = guest_stack_base;
	start->guest_stack_size = guest_stack_size;
	start->initial_guest_esp = guest_esp;

	error = pthread_attr_init(&attributes);
	if (error != 0)
	{
		free(start);
		(void)mac_guest_release(guest_stack_base, guest_stack_size);
		return error;
	}
	error = pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
	if (error == 0)
		error = pthread_create(&thread, &attributes, guest_thread_main, start);
	pthread_attr_destroy(&attributes);
	if (error != 0)
	{
		free(start);
		(void)mac_guest_release(guest_stack_base, guest_stack_size);
	}
	return error;
}
