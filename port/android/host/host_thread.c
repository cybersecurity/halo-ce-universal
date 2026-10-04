/*
HOST_THREAD.C

Threads that run guest code.

Guest (ILP32) code keeps stack addresses in 32-bit registers, so every
thread that runs it needs its stack in guest memory. Every such thread is
created here, with its stack given to pthread_create: the stack pointer
never leaves the thread's own stack, which is also the one ART checks on
every call into Java (SDL). The host's main thread and SDL's audio callback
(host_sdl.c) hand their work to threads made by host_native_thread_create.
The guest's thread pointer (its musl struct pthread) is kept per thread in
host TLS.

Thread stacks are freed by a reaper thread once the thread has fully exited.
*/

#include "host.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define GUARD_SIZE 0x4000

static __thread uint32_t guest_tp;

uint32_t host_get_tp(void)
{
	return guest_tp;
}

void host_set_tp(uint32_t thread)
{
	guest_tp = thread;
}

/* ---------- stacks */

static void *stack_allocate(size_t size, void **mapping, size_t *mapping_size)
{
	size_t total = size + GUARD_SIZE;
	void *base = host_low_map(total, PROT_READ | PROT_WRITE);

	if (!base)
		return NULL;
	/* guard page at the bottom */
	mprotect(base, GUARD_SIZE, PROT_NONE);
	*mapping = base;
	*mapping_size = total;
	return (char *)base + GUARD_SIZE;
}

static int on_guest_stack(void)
{
	uint64_t sp = (uint64_t)__builtin_frame_address(0);

#ifdef HALO_MACOS
	return (sp & ~UINT64_C(0xffffffff)) == HALO_MACOS_BIAS;
#else
	return sp < 0x100000000ULL;
#endif
}

/* ---------- calling into the guest */

typedef uint32_t (*guest_function)(uint32_t, uint32_t, uint32_t, uint32_t);

uint32_t host_call_guest(uint32_t function, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
	if (!on_guest_stack())
		host_fatal("guest code called on a thread without a guest stack");
	if (!guest_tp)
		((guest_function)guest_code_pointer(host_image.header->thread_attach))(0, 0, 0, 0);
	return ((guest_function)guest_code_pointer(function))(a, b, c, d);
}

void host_run_guest_main(uint32_t boot)
{
	((void (*)(uint32_t))guest_code_pointer(host_image.header->start))(boot);
	host_fatal("the guest returned from __guest_start");
}

/* ---------- guest threads */

struct thread_start
{
	void *(*function)(void *);
	void *argument;
	void *mapping;
	size_t mapping_size;
};

struct finished_thread
{
	struct finished_thread *next;
	pthread_t thread;
	void *mapping;
	size_t mapping_size;
};

static pthread_mutex_t reaper_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t reaper_condition = PTHREAD_COND_INITIALIZER;
static struct finished_thread *finished_threads;
static int reaper_started;

static void *reaper(void *unused)
{
	(void)unused;
	for (;;)
	{
		struct finished_thread *finished;

		pthread_mutex_lock(&reaper_lock);
		while (!finished_threads)
			pthread_cond_wait(&reaper_condition, &reaper_lock);
		finished = finished_threads;
		finished_threads = finished->next;
		pthread_mutex_unlock(&reaper_lock);
		pthread_join(finished->thread, NULL);
		host_low_unmap(finished->mapping, finished->mapping_size);
		free(finished);
	}
	return NULL;
}

static void *thread_main(void *context)
{
	struct thread_start start = *(struct thread_start *)context;
	struct finished_thread *finished;

	free(context);
	host_debug_thread_started();
	start.function(start.argument);
	host_debug_thread_exited();
	guest_tp = 0;

	finished = calloc(1, sizeof(*finished));
	finished->thread = pthread_self();
	finished->mapping = start.mapping;
	finished->mapping_size = start.mapping_size;
	pthread_mutex_lock(&reaper_lock);
	finished->next = finished_threads;
	finished_threads = finished;
	pthread_cond_signal(&reaper_condition);
	pthread_mutex_unlock(&reaper_lock);
	return NULL;
}

int host_native_thread_create(void *(*function)(void *), void *argument, size_t stack_size)
{
	struct thread_start *start = calloc(1, sizeof(*start));
	pthread_attr_t attributes;
	pthread_t thread;
	void *stack;
	int error;

	if (!start)
		return ENOMEM;
	pthread_mutex_lock(&reaper_lock);
	if (!reaper_started)
	{
		pthread_t reaper_thread;

		if (pthread_create(&reaper_thread, NULL, reaper, NULL) == 0)
		{
			pthread_detach(reaper_thread);
			reaper_started = 1;
		}
	}
	pthread_mutex_unlock(&reaper_lock);

	stack_size = (stack_size + 0xffff) & ~(size_t)0xffff;
	stack = stack_allocate(stack_size, &start->mapping, &start->mapping_size);
	if (!stack)
	{
		free(start);
		return EAGAIN;
	}
	start->function = function;
	start->argument = argument;
	pthread_attr_init(&attributes);
	pthread_attr_setstack(&attributes, stack, stack_size);
	error = pthread_create(&thread, &attributes, thread_main, start);
	pthread_attr_destroy(&attributes);
	if (error)
	{
		host_low_unmap(start->mapping, start->mapping_size);
		free(start);
	}
	return error;
}

static void *guest_thread_main(void *guest_thread)
{
	host_call_guest(host_image.header->thread_start, (uint32_t)(uintptr_t)guest_thread, 0, 0, 0);
	return NULL;
}

int host_thread_create(uint32_t guest_thread, uint32_t stack_size)
{
	return host_native_thread_create(guest_thread_main, (void *)(uintptr_t)guest_thread, stack_size);
}
