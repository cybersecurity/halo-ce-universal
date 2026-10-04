/*
HOST_DEBUG.C

A sampler for finding where the guest spends its time (or hangs) on a
device without root, where debuggerd cannot attach: with sample_seconds in
config.toml's [debug], every guest thread is interrupted that often and its program
counter, link register and frame chain are written to logcat. The addresses
symbolize against build/android/halo_guest.elf (llvm-symbolizer
--obj=build/android/halo_guest.elf 0x...).
*/

#include "host.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#define MAXIMUM_THREADS 64
#define SAMPLE_SIGNAL SIGURG

static pid_t guest_threads[MAXIMUM_THREADS];
static pthread_mutex_t threads_lock = PTHREAD_MUTEX_INITIALIZER;

void host_debug_thread_started(void)
{
	int index;

	pthread_mutex_lock(&threads_lock);
	for (index = 0; index < MAXIMUM_THREADS; index++)
	{
		if (!guest_threads[index])
		{
			guest_threads[index] = gettid();
			break;
		}
	}
	pthread_mutex_unlock(&threads_lock);
}

void host_debug_thread_exited(void)
{
	pid_t self = gettid();
	int index;

	pthread_mutex_lock(&threads_lock);
	for (index = 0; index < MAXIMUM_THREADS; index++)
	{
		if (guest_threads[index] == self)
			guest_threads[index] = 0;
	}
	pthread_mutex_unlock(&threads_lock);
}

static void sample_handler(int signal_number, siginfo_t *information, void *context)
{
	const ucontext_t *ucontext = context;
	const struct sigcontext *registers = (const struct sigcontext *)&ucontext->uc_mcontext;
	char line[512];
	int length, depth;
	uint64_t fp = registers->regs[29];

	(void)signal_number;
	(void)information;
	length = snprintf(line, sizeof(line), "sample tid %d: pc %llx lr %llx", gettid(),
		(unsigned long long)registers->pc, (unsigned long long)registers->regs[30]);
	for (depth = 0; depth < 12 && fp && fp < 0x100000000ULL && !(fp & 7) && host_low_owns(fp, 16); depth++)
	{
		const uint64_t *frame = (const uint64_t *)fp;

		length += snprintf(line + length, sizeof(line) - length, " %llx", (unsigned long long)frame[1]);
		if (frame[0] <= fp)
			break;
		fp = frame[0];
	}
	host_logf(HOST_LOG_INFO, "%s", line);
}

static void *sampler(void *context)
{
	unsigned int seconds = (unsigned int)(uintptr_t)context;

	for (;;)
	{
		pid_t threads[MAXIMUM_THREADS];
		int index;

		sleep(seconds);
		pthread_mutex_lock(&threads_lock);
		memcpy(threads, guest_threads, sizeof(threads));
		pthread_mutex_unlock(&threads_lock);
		for (index = 0; index < MAXIMUM_THREADS; index++)
		{
			if (threads[index])
				syscall(SYS_tgkill, getpid(), threads[index], SAMPLE_SIGNAL);
		}
	}
	return NULL;
}

void host_debug_start_sampler(const char *setting)
{
	struct sigaction action;
	pthread_t thread;
	unsigned int seconds = setting ? (unsigned int)atoi(setting) : 0;

	if (!seconds)
		return;
	memset(&action, 0, sizeof(action));
	action.sa_sigaction = sample_handler;
	action.sa_flags = SA_SIGINFO | SA_RESTART;
	sigemptyset(&action.sa_mask);
	sigaction(SAMPLE_SIGNAL, &action, NULL);
	if (pthread_create(&thread, NULL, sampler, (void *)(uintptr_t)seconds) == 0)
		pthread_detach(thread);
	host_logf(HOST_LOG_INFO, "sampling guest threads every %u s", seconds);
}
