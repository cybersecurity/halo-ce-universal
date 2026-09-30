/*
HOST_THREAD.C

Threads that run guest code.

Guest code keeps stack addresses in 32-bit registers, so every thread that
runs it needs its stack in guest memory. The native build's guest code
also needs the region's base in x28 (tools/macos_arm64_rebase.py), which
the trampolines here set: x28 is callee-saved, so it survives the guest's
calls into the host, and the guest never changes it. Threads the guest creates get
one from pthread_create (pthread_attr_setstack). The game's main thread is
the process's main thread, because Cocoa creates windows and delivers
events only there; host_run_guest_main moves its stack pointer to a stack in
guest memory instead. SDL's audio callback is handed to a thread that has a
guest stack (host_sdl.c).

The guest's thread pointer (its musl struct pthread) is kept per thread in
host TLS. Thread stacks are freed by a reaper thread once the thread has
fully exited.
*/

#include "host.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define GUARD_SIZE 0x4000
#define MAIN_STACK_SIZE (16 * 1024 * 1024)

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
	/* guard pages at the bottom */
	mprotect(base, GUARD_SIZE, PROT_NONE);
	*mapping = base;
	*mapping_size = total;
	return (char *)base + GUARD_SIZE;
}

static int on_guest_stack(void)
{
	uint64_t sp = (uint64_t)__builtin_frame_address(0);

	return sp >= host_guest_base && sp - host_guest_base < 0x100000000ULL;
}

/* ---------- calling into the guest */

#ifdef __aarch64__
/* calls the guest function at host address function with four 32-bit
arguments and x28 = the guest's base; the host's x27 and x28 are kept */
__attribute__((naked, noinline)) static uint32_t guest_call4(uint64_t function, uint64_t base, uint32_t a,
	uint32_t b, uint32_t c, uint32_t d)
{
	__asm__ volatile(
		"stp x29, x30, [sp, #-32]!\n\t"
		"mov x29, sp\n\t"
		"stp x27, x28, [sp, #16]\n\t"
		"mov x28, x1\n\t"
		"mov x9, x0\n\t"
		"mov w0, w2\n\t"
		"mov w1, w3\n\t"
		"mov w2, w4\n\t"
		"mov w3, w5\n\t"
		"blr x9\n\t"
		"ldp x27, x28, [sp, #16]\n\t"
		"ldp x29, x30, [sp], #32\n\t"
		"ret\n\t");
}

/* calls function(argument), a host function, with the stack pointer at
stack_top (16-byte aligned), and returns on the original stack */
__attribute__((naked, noinline)) static void call_on_stack(void (*function)(uint64_t), uint64_t argument,
	uint64_t stack_top)
{
	__asm__ volatile(
		"stp x29, x30, [sp, #-16]!\n\t"
		"mov x29, sp\n\t"
		"mov x9, sp\n\t"
		"mov sp, x2\n\t"
		"sub sp, sp, #16\n\t"
		"str x9, [sp]\n\t"
		"mov x9, x0\n\t"
		"mov x0, x1\n\t"
		"blr x9\n\t"
		"ldr x9, [sp]\n\t"
		"mov sp, x9\n\t"
		"ldp x29, x30, [sp], #16\n\t"
		"ret\n\t");
}

static uint32_t guest_call(uint32_t function, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
	return guest_call4(host_guest_base + function, host_guest_base, a, b, c, d);
}
#else
typedef uint32_t (*guest_function)(uint32_t, uint32_t, uint32_t, uint32_t);

static uint32_t guest_call(uint32_t function, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
	return ((guest_function)(uintptr_t)(host_guest_base + function))(a, b, c, d);
}

/* calls function(argument) with the stack pointer at stack_top (16-byte
aligned), and returns on the original stack */
__attribute__((naked, noinline)) static void call_on_stack(void (*function)(uint64_t), uint64_t argument,
	uint64_t stack_top)
{
	__asm__ volatile(
		"pushq %rbp\n\t"
		"movq %rsp, %rbp\n\t"
		"movq %rdx, %rsp\n\t"
		/* the old stack pointer (in rbp) on the new stack, keeping the
		call 16-byte aligned */
		"pushq %rbp\n\t"
		"subq $8, %rsp\n\t"
		"movq %rdi, %rax\n\t"
		"movq %rsi, %rdi\n\t"
		"callq *%rax\n\t"
		"addq $8, %rsp\n\t"
		"popq %rsp\n\t"
		"popq %rbp\n\t"
		"retq\n\t");
}
#endif

uint32_t host_call_guest(uint32_t function, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
	if (!on_guest_stack())
		host_fatal("guest code called on a thread without a guest stack");
	if (!guest_tp)
		guest_call(host_image.header->thread_attach, 0, 0, 0, 0);
	return guest_call(function, a, b, c, d);
}

static void run_guest_start(uint64_t boot)
{
	guest_call(host_image.header->start, (uint32_t)boot, 0, 0, 0);
}

void host_run_guest_main(uint32_t boot)
{
	void *mapping;
	size_t mapping_size;
	char *stack = stack_allocate(MAIN_STACK_SIZE, &mapping, &mapping_size);

	if (!stack)
		host_fatal("cannot allocate the game's stack");
	host_debug_thread_started();
	call_on_stack(run_guest_start, boot, ((uint64_t)(uintptr_t)(stack + MAIN_STACK_SIZE)) & ~15ULL);
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
