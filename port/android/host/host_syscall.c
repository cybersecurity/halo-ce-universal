/*
HOST_SYSCALL.C

System calls on behalf of the guest's musl runtime (its syscall_arch.h
sends every call here).

Most calls pass straight through: the guest's pointers are valid host
addresses, and its integer arguments arrive properly extended to 64 bits.
The exceptions are

- structures whose ILP32 layout differs from the kernel's: timespec and
  timeval (the guest's time_t is 32-bit, as in the MSVC runtime), iovec;
- memory mappings, which must stay below 4 GB (host_memory.c);
- the standard output and error streams, which go to logcat (on Android;
  on Linux arm64, port/linux/arm64, they are the process's);
- process exit, and calls that have no meaning for the guest (signal
  handlers, which the host owns).
*/

#include "host.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#ifdef __ANDROID__
#include <android/log.h>
#endif

/* the guest's structures */
struct guest_timespec
{
	int32_t seconds;
	int32_t nanoseconds;
};

struct guest_iovec
{
	uint32_t base;
	uint32_t length;
};

#define GUEST(type, value) ((type)(uintptr_t)(uint32_t)(value))

static int timespec_in(uint64_t address, struct timespec *result)
{
	const struct guest_timespec *value = GUEST(const struct guest_timespec *, address);

	if (!address)
		return 0;
	result->tv_sec = value->seconds;
	result->tv_nsec = value->nanoseconds;
	return 1;
}

static void timespec_out(uint64_t address, const struct timespec *value)
{
	struct guest_timespec *result = GUEST(struct guest_timespec *, address);

	if (!address)
		return;
	result->seconds = (int32_t)value->tv_sec;
	result->nanoseconds = (int32_t)value->tv_nsec;
}

static long result_of(long value)
{
	return value == -1 ? -errno : value;
}

/* ---------- standard output and error */

#ifdef __ANDROID__
struct log_stream
{
	char line[1024];
	size_t length;
};

static struct log_stream log_streams[2];
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

static void log_bytes(int fd, const char *bytes, size_t size)
{
	struct log_stream *stream = &log_streams[fd == 2];
	size_t index;

	pthread_mutex_lock(&log_lock);
	for (index = 0; index < size; index++)
	{
		char c = bytes[index];

		if (c == '\n' || stream->length == sizeof(stream->line) - 1)
		{
			stream->line[stream->length] = 0;
			__android_log_write(fd == 2 ? ANDROID_LOG_WARN : ANDROID_LOG_INFO, "halo", stream->line);
			stream->length = 0;
			if (c == '\n')
				continue;
		}
		stream->line[stream->length++] = c;
	}
	pthread_mutex_unlock(&log_lock);
}
#endif

static long guest_writev(int fd, uint64_t vector, int count, int64_t offset, int positional)
{
	struct iovec host_vector[64];
	const struct guest_iovec *guest_vector = GUEST(const struct guest_iovec *, vector);
	int index;

	if (count < 0 || count > 64)
		return -EINVAL;
	for (index = 0; index < count; index++)
	{
		host_vector[index].iov_base = GUEST(void *, guest_vector[index].base);
		host_vector[index].iov_len = guest_vector[index].length;
	}
#ifdef __ANDROID__
	if (fd == 1 || fd == 2)
	{
		long total = 0;

		for (index = 0; index < count; index++)
		{
			log_bytes(fd, host_vector[index].iov_base, host_vector[index].iov_len);
			total += (long)host_vector[index].iov_len;
		}
		return total;
	}
#endif
	if (positional)
		return result_of(pwritev(fd, host_vector, count, offset));
	return result_of(writev(fd, host_vector, count));
}

static long guest_readv(int fd, uint64_t vector, int count, int64_t offset, int positional)
{
	struct iovec host_vector[64];
	const struct guest_iovec *guest_vector = GUEST(const struct guest_iovec *, vector);
	int index;

	if (count < 0 || count > 64)
		return -EINVAL;
	for (index = 0; index < count; index++)
	{
		host_vector[index].iov_base = GUEST(void *, guest_vector[index].base);
		host_vector[index].iov_len = guest_vector[index].length;
	}
	if (positional)
		return result_of(preadv(fd, host_vector, count, offset));
	return result_of(readv(fd, host_vector, count));
}

/* ---------- time */

static long guest_futex(uint64_t address, int operation, uint32_t value, uint64_t timeout,
	uint64_t address2, uint32_t value3)
{
	struct timespec host_timeout;
	struct timespec *timeout_pointer = NULL;
	int command = operation & FUTEX_CMD_MASK;

	/* for these operations the fourth argument is a timeout; for the
	others it is a count passed as a pointer-sized integer */
	if (command == FUTEX_WAIT || command == FUTEX_WAIT_BITSET || command == FUTEX_LOCK_PI ||
		command == FUTEX_WAIT_REQUEUE_PI)
	{
		if (timespec_in(timeout, &host_timeout))
			timeout_pointer = &host_timeout;
		return result_of(syscall(SYS_futex, GUEST(void *, address), operation, value, timeout_pointer,
			GUEST(void *, address2), value3));
	}
	return result_of(syscall(SYS_futex, GUEST(void *, address), operation, value, (void *)(uintptr_t)timeout,
		GUEST(void *, address2), value3));
}

/* ---------- dispatch */

long long host_syscall(long long number, long long a, long long b, long long c,
	long long d, long long e, long long f)
{
	switch (number)
	{
	case SYS_write:
#ifdef __ANDROID__
		if (a == 1 || a == 2)
		{
			log_bytes((int)a, GUEST(const char *, b), (size_t)(uint32_t)c);
			return (uint32_t)c;
		}
#endif
		return result_of(write((int)a, GUEST(const void *, b), (size_t)(uint32_t)c));
	case SYS_writev:
		return guest_writev((int)a, (uint64_t)b, (int)c, 0, 0);
	case SYS_pwritev:
		return guest_writev((int)a, (uint64_t)b, (int)c, d, 1);
	case SYS_readv:
		return guest_readv((int)a, (uint64_t)b, (int)c, 0, 0);
	case SYS_preadv:
		return guest_readv((int)a, (uint64_t)b, (int)c, d, 1);

	case SYS_clock_gettime:
	case SYS_clock_getres:
	{
		struct timespec value;
		long result = syscall(number, (clockid_t)a, &value);

		if (result == 0)
			timespec_out((uint64_t)b, &value);
		return result_of(result);
	}
	case SYS_gettimeofday:
	{
		struct timespec value;
		struct guest_timespec *result = GUEST(struct guest_timespec *, a);

		clock_gettime(CLOCK_REALTIME, &value);
		if (result)
		{
			result->seconds = (int32_t)value.tv_sec;
			result->nanoseconds = (int32_t)(value.tv_nsec / 1000);
		}
		return 0;
	}
	case SYS_nanosleep:
	{
		struct timespec request, remaining;
		long result;

		if (!timespec_in((uint64_t)a, &request))
			return -EFAULT;
		result = result_of(nanosleep(&request, &remaining));
		if (result == -EINTR)
			timespec_out((uint64_t)b, &remaining);
		return result;
	}
	case SYS_clock_nanosleep:
	{
		struct timespec request, remaining;
		int result;

		if (!timespec_in((uint64_t)c, &request))
			return -EFAULT;
		result = clock_nanosleep((clockid_t)a, (int)b, &request, &remaining);
		if (result == EINTR)
			timespec_out((uint64_t)d, &remaining);
		return -result;
	}
	case SYS_futex:
		return guest_futex((uint64_t)a, (int)b, (uint32_t)c, (uint64_t)d, (uint64_t)e, (uint32_t)f);
	case SYS_ppoll:
	{
		struct timespec timeout;
		int has_timeout = timespec_in((uint64_t)c, &timeout);

		return result_of(ppoll(GUEST(struct pollfd *, a), (nfds_t)(uint32_t)b, has_timeout ? &timeout : NULL, NULL));
	}
	case SYS_utimensat:
	{
		struct timespec times[2];
		const struct guest_timespec *guest_times = GUEST(const struct guest_timespec *, c);

		if (guest_times)
		{
			timespec_in((uint64_t)c, &times[0]);
			timespec_in((uint64_t)c + sizeof(struct guest_timespec), &times[1]);
		}
		return result_of(utimensat((int)a, GUEST(const char *, b), guest_times ? times : NULL, (int)d));
	}

	case SYS_mmap:
		return host_guest_mmap((uint64_t)a, (uint64_t)b, (int)c, (int)d, (int)e, f);
	case SYS_munmap:
		return host_guest_munmap((uint64_t)a, (uint64_t)b);
	case SYS_mprotect:
		return host_guest_mprotect((uint64_t)a, (uint64_t)b, (int)c);
	case SYS_madvise:
		if ((uint64_t)a + (uint64_t)b > 0x100000000ULL)
			return -EINVAL;
		return result_of(syscall(SYS_madvise, a, b, c));
	case SYS_mremap:
	case SYS_brk:
		/* musl then falls back to mmap and copying */
		return -ENOMEM;

	case SYS_exit:
	case SYS_exit_group:
		host_exit((int)a);

	case SYS_set_tid_address:
		return gettid();
	case SYS_rt_sigaction:
	case SYS_sigaltstack:
		/* the host owns signal handling */
		return 0;
	case SYS_ioctl:
		return -ENOTTY;
	case SYS_statx:
	case SYS_statfs:
	case SYS_fstatfs:
	case SYS_clone:
	case SYS_clone3:
	case SYS_execve:
	case SYS_rt_sigtimedwait:
	case SYS_pselect6:
	case SYS_epoll_pwait:
	case SYS_sysinfo:
	case SYS_sendmsg:
	case SYS_recvmsg:
	case SYS_timer_create:
	case SYS_timer_settime:
	case SYS_timer_gettime:
	case SYS_setitimer:
	case SYS_getitimer:
	case SYS_times:
	case SYS_getrusage:
	case SYS_wait4:
	case SYS_waitid:
		return -ENOSYS;

	case SYS_read:
	case SYS_openat:
	case SYS_close:
	case SYS_lseek:
	case SYS_pread64:
	case SYS_pwrite64:
	case SYS_getdents64:
	case SYS_unlinkat:
	case SYS_renameat:
	case SYS_renameat2:
	case SYS_mkdirat:
	case SYS_fchmod:
	case SYS_fchmodat:
	case SYS_ftruncate:
	case SYS_fsync:
	case SYS_fdatasync:
	case SYS_fcntl:
	case SYS_getcwd:
	case SYS_chdir:
	case SYS_readlinkat:
	case SYS_faccessat:
	case SYS_dup:
	case SYS_dup3:
	case SYS_pipe2:
	case SYS_fstat:
	case SYS_newfstatat:
	case SYS_getpid:
	case SYS_getppid:
	case SYS_gettid:
	case SYS_getuid:
	case SYS_geteuid:
	case SYS_getgid:
	case SYS_getegid:
	case SYS_sched_yield:
	case SYS_getrandom:
	case SYS_kill:
	case SYS_tkill:
	case SYS_tgkill:
	case SYS_rt_sigprocmask:
	case SYS_uname:
	case SYS_prlimit64:
	case SYS_getrlimit:
	case SYS_umask:
	case SYS_flock:
	case SYS_membarrier:
		return result_of(syscall(number, a, b, c, d, e, f));

	default:
		host_logf(HOST_LOG_WARN, "guest system call %lld is not supported", number);
		return -ENOSYS;
	}
}
