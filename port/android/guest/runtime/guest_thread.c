/*
GUEST_THREAD.C

Threads for the guest's musl runtime.

musl finds the current thread's struct pthread through the thread pointer
register, which in this process belongs to the host's bionic. The guest's
thread pointer is instead kept by the host per thread (host_get_tp), and
every guest thread is started by the host on a stack inside guest memory
(host_thread_create), since ILP32 code cannot address a stack above 4 GB.

musl's own synchronisation (mutexes, condition variables, stdio and malloc
locks) is used unchanged: it is built on futexes, which the host passes
through to the kernel, and the guest's addresses are valid host addresses.
Thread creation, joining and detaching are implemented here, together with
the emulated thread-local storage clang generates under -femulated-tls.
*/

#include "pthread_impl.h"
#include "stdio_impl.h"
#include "lock.h"
#include "guest_host.h"

#include <stdlib.h>
#include <string.h>

struct guest_thread
{
	struct pthread pthread;
	void *(*start)(void *);
	void *argument;
	/* _thread_running, then _thread_exited once start returns; or
	_thread_detached if detached first */
	volatile int state;
	void **emutls;
	size_t emutls_count;
};

enum
{
	_thread_running = 1,
	_thread_detached,
	_thread_exited,
};

/* (musl's pthread_impl.h has its own, 128 KB: the game's threads need more) */
#undef DEFAULT_STACK_SIZE
#define DEFAULT_STACK_SIZE (1024 * 1024)

static struct guest_thread main_thread;

uintptr_t __guest_get_tp(void)
{
	return host_get_tp();
}

static void thread_initialize(struct guest_thread *thread)
{
	struct pthread *td = &thread->pthread;

	td->self = td;
	td->prev = td->next = td;
	td->detach_state = DT_JOINABLE;
	td->locale = &libc.global_locale;
	td->robust_list.head = &td->robust_list.head;
	thread->state = _thread_running;
}

/* called by __guest_start on the main thread before anything else */
void __guest_thread_initialize_main(void)
{
	thread_initialize(&main_thread);
	host_set_tp((unsigned int)&main_thread);
	main_thread.pthread.tid = __syscall(SYS_gettid);
	libc.can_do_threads = 1;
	/* lock stdio from the start: the platform layer logs from several
	threads, and the standard streams are otherwise never locked */
	libc.threaded = 1;
	stdin->lock = 0;
	stdout->lock = 0;
	stderr->lock = 0;
}

static void thread_free(struct guest_thread *thread)
{
	size_t index;

	for (index = 0; index < thread->emutls_count; index++)
		free(thread->emutls[index]);
	free(thread->emutls);
	free(thread);
}

/* the host calls this (image header) on the new thread's guest stack */
void __guest_thread_start(unsigned int handle)
{
	struct guest_thread *thread = (struct guest_thread *)handle;
	void *result;

	host_set_tp(handle);
	thread->pthread.tid = __syscall(SYS_gettid);
	result = thread->start(thread->argument);
	thread->pthread.result = result;
	if (a_cas(&thread->state, _thread_running, _thread_exited) == _thread_detached)
	{
		thread_free(thread);
	}
	else
	{
		__wake(&thread->state, -1, 1);
	}
	host_set_tp(0);
}

/* a host thread (the audio callback's, for instance) is about to run
guest code: give it a struct pthread */
unsigned int __guest_thread_attach(void)
{
	struct guest_thread *thread = calloc(1, sizeof(*thread));

	if (!thread)
		host_abort("cannot allocate a guest thread");
	thread_initialize(thread);
	thread->state = _thread_detached;
	host_set_tp((unsigned int)thread);
	thread->pthread.tid = __syscall(SYS_gettid);
	return (unsigned int)thread;
}

int pthread_create(pthread_t *restrict result, const pthread_attr_t *restrict attributes,
	void *(*start)(void *), void *restrict argument)
{
	struct guest_thread *thread = calloc(1, sizeof(*thread));
	size_t stack_size = DEFAULT_STACK_SIZE;
	int error;

	if (!thread)
		return EAGAIN;
	thread_initialize(thread);
	thread->start = start;
	thread->argument = argument;
	if (attributes)
	{
		if (attributes->_a_stacksize > stack_size)
			stack_size = attributes->_a_stacksize;
		if (attributes->_a_detach)
		{
			thread->state = _thread_detached;
			thread->pthread.detach_state = DT_DETACHED;
		}
	}
	a_inc(&libc.threads_minus_1);
	libc.need_locks = 1;
	*result = &thread->pthread;
	error = host_thread_create((unsigned int)thread, (unsigned int)stack_size);
	if (error)
	{
		a_dec(&libc.threads_minus_1);
		free(thread);
		return error;
	}
	return 0;
}

int pthread_join(pthread_t handle, void **result)
{
	struct guest_thread *thread = (struct guest_thread *)handle;
	int state;

	while ((state = thread->state) != _thread_exited)
	{
		if (state == _thread_detached)
			return EINVAL;
		__wait(&thread->state, 0, state, 1);
	}
	if (result)
		*result = thread->pthread.result;
	a_dec(&libc.threads_minus_1);
	thread_free(thread);
	return 0;
}

int pthread_detach(pthread_t handle)
{
	struct guest_thread *thread = (struct guest_thread *)handle;

	if (a_cas(&thread->state, _thread_running, _thread_detached) == _thread_exited)
	{
		a_dec(&libc.threads_minus_1);
		thread_free(thread);
	}
	return 0;
}

void pthread_exit(void *result)
{
	(void)result;
	host_abort("pthread_exit is not supported by the Android guest runtime");
}

/* ---------- emulated TLS (clang -femulated-tls) */

struct __emutls_control
{
	size_t size;
	size_t align;
	union
	{
		uintptr_t index;
		void *address;
	} object;
	void *value;
};

static volatile int emutls_lock[1];
static uintptr_t emutls_next_index = 1;

void *__emutls_get_address(struct __emutls_control *control)
{
	struct guest_thread *thread = (struct guest_thread *)__pthread_self();
	uintptr_t index = __atomic_load_n(&control->object.index, __ATOMIC_ACQUIRE);
	void *storage;

	if (!index)
	{
		__lock(emutls_lock);
		index = control->object.index;
		if (!index)
		{
			index = emutls_next_index++;
			__atomic_store_n(&control->object.index, index, __ATOMIC_RELEASE);
		}
		__unlock(emutls_lock);
	}
	if (index > thread->emutls_count)
	{
		size_t count = index + 16;
		void **slots = realloc(thread->emutls, count * sizeof(void *));

		if (!slots)
			host_abort("cannot grow emulated TLS");
		memset(slots + thread->emutls_count, 0, (count - thread->emutls_count) * sizeof(void *));
		thread->emutls = slots;
		thread->emutls_count = count;
	}
	storage = thread->emutls[index - 1];
	if (!storage)
	{
		size_t align = control->align > sizeof(void *) ? control->align : sizeof(void *);

		storage = aligned_alloc(align, (control->size + align - 1) & ~(align - 1));
		if (!storage)
			host_abort("cannot allocate emulated TLS");
		if (control->value)
			memcpy(storage, control->value, control->size);
		else
			memset(storage, 0, control->size);
		thread->emutls[index - 1] = storage;
	}
	return storage;
}
