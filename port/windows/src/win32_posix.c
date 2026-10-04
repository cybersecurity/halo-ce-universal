/*
WIN32_POSIX.C

The POSIX calls the platform layer shared with Linux makes, implemented with
Windows (the headers are in port/windows/include/posix): threads, mutexes and
condition variables, clocks and sleeping, sysconf, and the memory mapping the
Xbox memory window uses. Also the process start-up the Windows build needs.
*/

#include <windows.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <timeapi.h>

#include "pthread.h"
#include "sched.h"
#include "sys/mman.h"
#include "time.h"
#include "unistd.h"

/* ---------- start-up */

/* laptops with a second, faster GPU (NVIDIA Optimus, AMD switchable
graphics) run a program on the integrated one unless it exports these */
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;

__attribute__((constructor))
static void windows_startup(void)
{
	/* files are binary unless opened otherwise, as on the Xbox (the C
	runtime's default text mode would translate line ends) */
	_set_fmode(_O_BINARY);
	/* Sleep() waits in 1 ms steps rather than the default 15.6 ms: the
	game's frame pacing sleeps for short intervals */
	timeBeginPeriod(1);
	/* a release build has no console (/SUBSYSTEM:WINDOWS), so the platform
	layer's log (platform_log's stderr: invite links, the data root, crash
	reports) goes to halo.log next to halo.exe, unless stderr already goes
	somewhere (a pipe or a file the game was started with) */
	if (_fileno(stderr) < 0)
	{
		char path[MAX_PATH];
		DWORD length = GetModuleFileNameA(NULL, path, sizeof(path));
		char *slash;

		if (length && length < sizeof(path) && (slash = strrchr(path, '\\')) &&
			(size_t)(slash + 1 - path) + sizeof("halo.log") <= sizeof(path))
		{
			strcpy(slash + 1, "halo.log");
			if (freopen(path, "w", stderr))
				setvbuf(stderr, NULL, _IONBF, 0);
		}
	}
}

static int errno_from_windows_error(DWORD error)
{
	switch (error)
	{
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:
	case ERROR_INVALID_DRIVE:
		return ENOENT;
	case ERROR_ACCESS_DENIED:
	case ERROR_SHARING_VIOLATION:
	case ERROR_LOCK_VIOLATION:
		return EACCES;
	case ERROR_ALREADY_EXISTS:
	case ERROR_FILE_EXISTS:
		return EEXIST;
	case ERROR_NOT_ENOUGH_MEMORY:
	case ERROR_OUTOFMEMORY:
	case ERROR_COMMITMENT_LIMIT:
		return ENOMEM;
	case ERROR_INVALID_ADDRESS:
	case ERROR_INVALID_PARAMETER:
		return EINVAL;
	default:
		return EIO;
	}
}

/* ---------- threads */

struct thread_start
{
	void *(*start)(void *);
	void *argument;
};

static DWORD WINAPI thread_main(LPVOID parameter)
{
	struct thread_start start = *(struct thread_start *)parameter;

	free(parameter);
	start.start(start.argument);
	return 0;
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attributes, void *(*start)(void *), void *argument)
{
	struct thread_start *parameter = malloc(sizeof(*parameter));
	DWORD identifier;
	HANDLE handle;

	if (!parameter)
		return EAGAIN;
	parameter->start = start;
	parameter->argument = argument;
	handle = CreateThread(NULL, attributes ? attributes->stack_size : 0, thread_main, parameter,
		STACK_SIZE_PARAM_IS_A_RESERVATION, &identifier);
	if (!handle)
	{
		free(parameter);
		return EAGAIN;
	}
	/* nothing joins threads: the handle is not needed */
	CloseHandle(handle);
	*thread = identifier;
	return 0;
}

int pthread_detach(pthread_t thread)
{
	(void)thread;
	return 0;
}

pthread_t pthread_self(void)
{
	return GetCurrentThreadId();
}

int pthread_equal(pthread_t thread1, pthread_t thread2)
{
	return thread1 == thread2;
}

int pthread_attr_init(pthread_attr_t *attributes)
{
	attributes->stack_size = 0;
	attributes->detached = 0;
	return 0;
}

int pthread_attr_destroy(pthread_attr_t *attributes)
{
	(void)attributes;
	return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t *attributes, int state)
{
	attributes->detached = state;
	return 0;
}

int pthread_attr_setstacksize(pthread_attr_t *attributes, size_t size)
{
	attributes->stack_size = size;
	return 0;
}

/* ---------- mutexes */

int pthread_mutexattr_init(pthread_mutexattr_t *attributes)
{
	attributes->type = PTHREAD_MUTEX_DEFAULT;
	return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *attributes)
{
	(void)attributes;
	return 0;
}

int pthread_mutexattr_settype(pthread_mutexattr_t *attributes, int type)
{
	attributes->type = type;
	return 0;
}

/* the critical section behind a mutex, created on first use so that
PTHREAD_MUTEX_INITIALIZER can be a constant */
static CRITICAL_SECTION *mutex_section(pthread_mutex_t *mutex)
{
	CRITICAL_SECTION *section = mutex->section;

	if (!section)
	{
		CRITICAL_SECTION *created = malloc(sizeof(*created));

		if (!created)
			abort();
		InitializeCriticalSection(created);
		section = InterlockedCompareExchangePointer(&mutex->section, created, NULL);
		if (section)
		{
			DeleteCriticalSection(created);
			free(created);
		}
		else
		{
			section = created;
		}
	}
	return section;
}

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attributes)
{
	(void)attributes;
	mutex->section = NULL;
	mutex_section(mutex);
	return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
	CRITICAL_SECTION *section = mutex->section;

	if (section)
	{
		DeleteCriticalSection(section);
		free(section);
		mutex->section = NULL;
	}
	return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex)
{
	EnterCriticalSection(mutex_section(mutex));
	return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *mutex)
{
	return TryEnterCriticalSection(mutex_section(mutex)) ? 0 : EBUSY;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
	LeaveCriticalSection(mutex_section(mutex));
	return 0;
}

/* ---------- condition variables */

_Static_assert(sizeof(CONDITION_VARIABLE) == sizeof(void *), "pthread_cond_t holds a CONDITION_VARIABLE");

int pthread_cond_init(pthread_cond_t *condition, const pthread_condattr_t *attributes)
{
	(void)attributes;
	InitializeConditionVariable((PCONDITION_VARIABLE)&condition->variable);
	return 0;
}

int pthread_cond_destroy(pthread_cond_t *condition)
{
	(void)condition;
	return 0;
}

int pthread_cond_wait(pthread_cond_t *condition, pthread_mutex_t *mutex)
{
	SleepConditionVariableCS((PCONDITION_VARIABLE)&condition->variable, mutex_section(mutex), INFINITE);
	return 0;
}

int pthread_cond_timedwait(pthread_cond_t *condition, pthread_mutex_t *mutex, const struct timespec *deadline)
{
	struct timespec now;
	long long milliseconds;

	clock_gettime(CLOCK_REALTIME, &now);
	milliseconds = ((long long)deadline->tv_sec - now.tv_sec) * 1000 + (deadline->tv_nsec - now.tv_nsec) / 1000000;
	if (milliseconds < 0)
		milliseconds = 0;
	if (!SleepConditionVariableCS((PCONDITION_VARIABLE)&condition->variable, mutex_section(mutex),
		milliseconds >= INFINITE ? INFINITE - 1 : (DWORD)milliseconds))
	{
		return GetLastError() == ERROR_TIMEOUT ? ETIMEDOUT : EINVAL;
	}
	return 0;
}

int pthread_cond_signal(pthread_cond_t *condition)
{
	WakeConditionVariable((PCONDITION_VARIABLE)&condition->variable);
	return 0;
}

int pthread_cond_broadcast(pthread_cond_t *condition)
{
	WakeAllConditionVariable((PCONDITION_VARIABLE)&condition->variable);
	return 0;
}

/* ---------- time */

int clock_gettime(clockid_t clock, struct timespec *time)
{
	if (clock == CLOCK_MONOTONIC)
	{
		static LARGE_INTEGER frequency;
		LARGE_INTEGER counter;

		if (!frequency.QuadPart)
			QueryPerformanceFrequency(&frequency);
		QueryPerformanceCounter(&counter);
		time->tv_sec = (time_t)(counter.QuadPart / frequency.QuadPart);
		time->tv_nsec = (long)((counter.QuadPart % frequency.QuadPart) * 1000000000LL / frequency.QuadPart);
	}
	else
	{
		/* 100 ns intervals since 1601 */
		FILETIME now;
		unsigned long long intervals;

		GetSystemTimePreciseAsFileTime(&now);
		intervals = ((unsigned long long)now.dwHighDateTime << 32) | now.dwLowDateTime;
		intervals -= 116444736000000000ULL;
		time->tv_sec = (time_t)(intervals / 10000000ULL);
		time->tv_nsec = (long)(intervals % 10000000ULL) * 100;
	}
	return 0;
}

int nanosleep(const struct timespec *duration, struct timespec *remaining)
{
	long long milliseconds = (long long)duration->tv_sec * 1000 + (duration->tv_nsec + 999999) / 1000000;

	Sleep(milliseconds > 0x7fffffff ? 0x7fffffff : (DWORD)milliseconds);
	if (remaining)
	{
		remaining->tv_sec = 0;
		remaining->tv_nsec = 0;
	}
	return 0;
}

int clock_nanosleep(clockid_t clock, int flags, const struct timespec *time, struct timespec *remaining)
{
	struct timespec now;
	long long nanoseconds;

	if (!(flags & TIMER_ABSTIME))
		return nanosleep(time, remaining);
	/* sleep until about a millisecond before the deadline (Sleep rounds
	up to the timer period), then yield until it passes */
	for (;;)
	{
		clock_gettime(clock, &now);
		nanoseconds = ((long long)time->tv_sec - now.tv_sec) * 1000000000LL + (time->tv_nsec - now.tv_nsec);
		if (nanoseconds <= 0)
			return 0;
		if (nanoseconds > 2000000)
			Sleep((DWORD)(nanoseconds / 1000000 - 1));
		else
			SwitchToThread();
	}
}

int sched_yield(void)
{
	SwitchToThread();
	return 0;
}

long sysconf(int name)
{
	SYSTEM_INFO system;
	MEMORYSTATUSEX memory;

	GetSystemInfo(&system);
	switch (name)
	{
	case _SC_PAGESIZE:
		return (long)system.dwPageSize;
	case _SC_NPROCESSORS_ONLN:
		return (long)system.dwNumberOfProcessors;
	case _SC_PHYS_PAGES:
	case _SC_AVPHYS_PAGES:
		memory.dwLength = sizeof(memory);
		if (!GlobalMemoryStatusEx(&memory))
			return -1;
		return (long)((name == _SC_PHYS_PAGES ? memory.ullTotalPhys : memory.ullAvailPhys) / system.dwPageSize);
	default:
		errno = EINVAL;
		return -1;
	}
}

/* ---------- files */

static ssize_t positioned_transfer(int descriptor, void *buffer, size_t count, off_t offset, BOOL write)
{
	HANDLE handle = (HANDLE)_get_osfhandle(descriptor);
	OVERLAPPED overlapped;
	DWORD transferred = 0;
	BOOL result;

	if (handle == INVALID_HANDLE_VALUE)
	{
		errno = EBADF;
		return -1;
	}
	memset(&overlapped, 0, sizeof(overlapped));
	overlapped.Offset = (DWORD)offset;
	overlapped.OffsetHigh = (DWORD)((unsigned long long)offset >> 32);
	result = write ?
		WriteFile(handle, buffer, (DWORD)count, &transferred, &overlapped) :
		ReadFile(handle, buffer, (DWORD)count, &transferred, &overlapped);
	if (!result)
	{
		if (!write && GetLastError() == ERROR_HANDLE_EOF)
			return 0;
		errno = errno_from_windows_error(GetLastError());
		return -1;
	}
	return (ssize_t)transferred;
}

ssize_t pread(int descriptor, void *buffer, size_t count, off_t offset)
{
	return positioned_transfer(descriptor, buffer, count, offset, FALSE);
}

ssize_t pwrite(int descriptor, const void *buffer, size_t count, off_t offset)
{
	return positioned_transfer(descriptor, (void *)buffer, count, offset, TRUE);
}

ssize_t readlink(const char *path, char *buffer, size_t size)
{
	DWORD length;
	DWORD index;

	if (strcmp(path, "/proc/self/exe") != 0)
	{
		errno = EINVAL;
		return -1;
	}
	length = GetModuleFileNameA(NULL, buffer, (DWORD)size);
	if (!length || length >= size)
	{
		errno = ENAMETOOLONG;
		return -1;
	}
	for (index = 0; index < length; index++)
	{
		if (buffer[index] == '\\')
			buffer[index] = '/';
	}
	return (ssize_t)length;
}

int pause(void)
{
	Sleep(INFINITE);
	return -1;
}

/* ---------- memory mapping */

static DWORD windows_protection(int protection)
{
	switch (protection & (PROT_READ | PROT_WRITE | PROT_EXEC))
	{
	case PROT_NONE: return PAGE_NOACCESS;
	case PROT_READ: return PAGE_READONLY;
	case PROT_EXEC: return PAGE_EXECUTE;
	case PROT_READ | PROT_EXEC: return PAGE_EXECUTE_READ;
	case PROT_READ | PROT_WRITE | PROT_EXEC:
	case PROT_WRITE | PROT_EXEC: return PAGE_EXECUTE_READWRITE;
	default: return PAGE_READWRITE;
	}
}

static BOOL address_reserved(void *address)
{
	MEMORY_BASIC_INFORMATION information;

	return address && VirtualQuery(address, &information, sizeof(information)) &&
		information.State != MEM_FREE;
}

void *mmap(void *address, size_t length, int protection, int flags, int descriptor, long offset)
{
	void *result;

	(void)descriptor;
	(void)offset;
	if (!(flags & MAP_ANONYMOUS))
	{
		errno = ENODEV;
		return MAP_FAILED;
	}
	if (protection == PROT_NONE && (flags & MAP_NORESERVE))
	{
		if ((flags & MAP_FIXED) && address_reserved(address))
		{
			/* give pages inside a reservation back */
			if (!VirtualFree(address, length, MEM_DECOMMIT))
			{
				errno = errno_from_windows_error(GetLastError());
				return MAP_FAILED;
			}
			return address;
		}
		result = VirtualAlloc(address, length, MEM_RESERVE, PAGE_NOACCESS);
	}
	else if ((flags & MAP_FIXED) && address_reserved(address))
	{
		/* fresh zeroed pages inside a reservation: committing pages that
		are already committed would keep their contents */
		VirtualFree(address, length, MEM_DECOMMIT);
		result = VirtualAlloc(address, length, MEM_COMMIT, windows_protection(protection));
	}
	else
	{
		result = VirtualAlloc(address, length, MEM_RESERVE | MEM_COMMIT, windows_protection(protection));
	}
	if (!result)
	{
		errno = errno_from_windows_error(GetLastError());
		return MAP_FAILED;
	}
	return result;
}

int munmap(void *address, size_t length)
{
	MEMORY_BASIC_INFORMATION information;

	if (!VirtualQuery(address, &information, sizeof(information)))
	{
		errno = EINVAL;
		return -1;
	}
	if (information.AllocationBase == address &&
		!VirtualFree(address, 0, MEM_RELEASE))
	{
		errno = errno_from_windows_error(GetLastError());
		return -1;
	}
	if (information.AllocationBase != address &&
		!VirtualFree(address, length, MEM_DECOMMIT))
	{
		errno = errno_from_windows_error(GetLastError());
		return -1;
	}
	return 0;
}

int mprotect(void *address, size_t length, int protection)
{
	DWORD previous;

	if (!VirtualProtect(address, length, windows_protection(protection), &previous))
	{
		errno = errno_from_windows_error(GetLastError());
		return -1;
	}
	return 0;
}
