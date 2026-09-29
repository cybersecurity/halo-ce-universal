#include "guest_callback.h"

#include "guest_allocator.h"
#include "guest_call.h"
#include "host_services.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>

#define GUEST_CALLBACK_RETURN UINT32_C(0xffffffff)
#define GUEST_CALLBACK_MAX_ARGS 8u
#define GUEST_CALLBACK_STACK_ALIGNMENT 16u

struct mac_guest_callback_context
{
	struct mac_guest_thread_runtime runtime;
	pthread_t owner;
	uint32_t guest_thread_va;
	uint32_t stack_base;
	uint32_t stack_top;
	size_t stack_size;
	int attached;
	int invoking;
};

static struct mac_guest_thread_runtime installed_runtime;
static uint32_t installed_attach_va;
static int runtime_installed;
static pthread_mutex_t runtime_mutex = PTHREAD_MUTEX_INITIALIZER;

int mac_guest_callback_runtime_install(const struct mac_guest_thread_runtime *runtime,
	uint32_t guest_thread_attach_va)
{
	int result = -1;
	if (!runtime || !runtime->initialize || !runtime->dispatch || !guest_thread_attach_va)
		return -1;
	pthread_mutex_lock(&runtime_mutex);
	if (!runtime_installed)
	{
		installed_runtime = *runtime;
		installed_attach_va = guest_thread_attach_va;
		runtime_installed = 1;
		result = 0;
	}
	pthread_mutex_unlock(&runtime_mutex);
	return result;
}

static int current_runtime(struct mac_guest_thread_runtime *runtime,
	uint32_t *attach_va)
{
	pthread_mutex_lock(&runtime_mutex);
	if (runtime_installed)
	{
		*runtime = installed_runtime;
		*attach_va = installed_attach_va;
	}
	pthread_mutex_unlock(&runtime_mutex);
	return runtime_installed ? 0 : -1;
}

static int make_frame(struct mac_guest_callback_context *context,
	const uint32_t *arguments, size_t argument_count, uint32_t *guest_esp_out)
{
	uint32_t frame[GUEST_CALLBACK_MAX_ARGS + 1];
	uint64_t bytes = (argument_count + 1) * sizeof(uint32_t);
	uint32_t guest_esp;

	if (argument_count > GUEST_CALLBACK_MAX_ARGS ||
		(argument_count && !arguments) || bytes > context->stack_size)
		return EINVAL;
	{
		uint32_t candidate = context->stack_top - (uint32_t)bytes;
		guest_esp = (candidate & ~UINT32_C(15)) + 12u;
		if (guest_esp > candidate)
			guest_esp -= 16u;
	}
	/* An i386 SysV call enters with ESP == 12 (mod 16): the caller's aligned
	 * stack held args, then CALL pushed the four-byte return address. */
	if (guest_esp < context->stack_base ||
		(uint64_t)guest_esp + bytes > context->stack_top ||
		(guest_esp & 15u) != 12u)
		return EINVAL;
	frame[0] = GUEST_CALLBACK_RETURN;
	for (size_t index = 0; index < argument_count; ++index)
		frame[index + 1] = arguments[index];
	if (mac_guest_write(guest_esp, frame, (size_t)bytes) != 0)
		return EFAULT;
	*guest_esp_out = guest_esp;
	return 0;
}

int mac_guest_callback_context_create_current(size_t guest_stack_size,
	struct mac_guest_callback_context **context_out)
{
	struct mac_guest_callback_context *context;
	struct mac_guest_thread_runtime runtime;
	uint32_t attach_va, esp;
	uint32_t frame = GUEST_CALLBACK_RETURN;
	uint64_t end;
	int error;

	if (!context_out || guest_stack_size < 64u * 1024u ||
		current_runtime(&runtime, &attach_va) != 0 || mac_host_get_guest_tp() != 0)
		return EINVAL;
	context = calloc(1, sizeof(*context));
	if (!context)
		return ENOMEM;
	error = mac_guest_allocate(guest_stack_size, 16u * 1024u, &context->stack_base);
	if (error != 0)
	{
		free(context);
		return error == ENOMEM ? ENOMEM : EAGAIN;
	}
	end = (uint64_t)context->stack_base + guest_stack_size;
	if (end > UINT32_MAX)
	{
		(void)mac_guest_release(context->stack_base, guest_stack_size);
		free(context);
		return EOVERFLOW;
	}
	context->stack_size = guest_stack_size;
	context->stack_top = (uint32_t)end & ~(GUEST_CALLBACK_STACK_ALIGNMENT - 1u);
	context->runtime = runtime;
	context->owner = pthread_self();
	if (context->stack_top < context->stack_base + sizeof(frame))
	{
		(void)mac_guest_release(context->stack_base, guest_stack_size);
		free(context);
		return EOVERFLOW;
	}
	esp = context->stack_top - sizeof(frame);
	if ((esp & 15u) != 12u || mac_guest_write(esp, &frame, sizeof(frame)) != 0)
	{
		(void)mac_guest_release(context->stack_base, guest_stack_size);
		free(context);
		return EFAULT;
	}
	context->runtime.initialize(context->runtime.context, 0, esp);
	context->runtime.dispatch(context->runtime.context, attach_va);
	context->guest_thread_va = mac_host_get_guest_tp();
	if (!context->guest_thread_va)
	{
		(void)mac_guest_release(context->stack_base, guest_stack_size);
		free(context);
		return EFAULT;
	}
	context->attached = 1;
	*context_out = context;
	return 0;
}

int mac_guest_callback_invoke(struct mac_guest_callback_context *context,
	uint32_t guest_function_va, const uint32_t *arguments, size_t argument_count)
{
	uint32_t esp;
	int error;

	if (!context || !context->attached || !guest_function_va ||
		!pthread_equal(context->owner, pthread_self()) || context->invoking)
		return EINVAL;
	error = make_frame(context, arguments, argument_count, &esp);
	if (error != 0)
		return error;
	context->invoking = 1;
	context->runtime.initialize(context->runtime.context, context->guest_thread_va, esp);
	context->runtime.dispatch(context->runtime.context, guest_function_va);
	context->invoking = 0;
	return 0;
}

void mac_guest_callback_context_destroy_current(struct mac_guest_callback_context *context)
{
	if (!context)
		return;
	if (!pthread_equal(context->owner, pthread_self()))
		mac_host_abort("guest callback context destroyed from a different thread");
	if (context->invoking)
		mac_host_abort("guest callback context destroyed during a callback");
	if (context->attached)
		mac_host_set_guest_tp(0);
	(void)mac_guest_release(context->stack_base, context->stack_size);
	free(context);
}
