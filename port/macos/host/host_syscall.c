/*
HOST_SYSCALL.C

System calls on behalf of the guest's musl runtime (its syscall_arch.h sends
every call here, with the Linux asm-generic numbers of
port/macos/guest/libc/arch/x32/bits/syscall.h.in).

The guest's musl is a Linux C library, and this is macOS: each call is
performed with the macOS equivalent, converting

- flag and constant values (open flags, AT_* values, clocks, fcntl
  commands) and errno values, which differ between the two systems;
- structures: the guest's ILP32 timespec, timeval and iovec, and the
  kernel's struct stat and linux_dirent64 that musl expects;
- directory reading (getdents64), which macOS only offers as readdir;
- futexes, which macOS offers as os_sync_wait_on_address;
- memory mappings, which must stay below 4 GB (host_memory.c).
*/

#include "host.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <os/os_sync_wait_on_address.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include "../guest/libc/arch/x32/bits/syscall.h.in"


/* ---------- errno values */

int host_linux_errno(int value)
{
	switch (value)
	{
	case EDEADLK: return 35;
	case EAGAIN: return 11;
	case EINPROGRESS: return 115;
	case EALREADY: return 114;
	case ENOTSOCK: return 88;
	case EDESTADDRREQ: return 89;
	case EMSGSIZE: return 90;
	case EPROTOTYPE: return 91;
	case ENOPROTOOPT: return 92;
	case EPROTONOSUPPORT: return 93;
	case ESOCKTNOSUPPORT: return 94;
	case ENOTSUP: return 95;
	case EOPNOTSUPP: return 95;
	case EPFNOSUPPORT: return 96;
	case EAFNOSUPPORT: return 97;
	case EADDRINUSE: return 98;
	case EADDRNOTAVAIL: return 99;
	case ENETDOWN: return 100;
	case ENETUNREACH: return 101;
	case ENETRESET: return 102;
	case ECONNABORTED: return 103;
	case ECONNRESET: return 104;
	case ENOBUFS: return 105;
	case EISCONN: return 106;
	case ENOTCONN: return 107;
	case ESHUTDOWN: return 108;
	case ETOOMANYREFS: return 109;
	case ETIMEDOUT: return 110;
	case ECONNREFUSED: return 111;
	case ELOOP: return 40;
	case ENAMETOOLONG: return 36;
	case EHOSTDOWN: return 112;
	case EHOSTUNREACH: return 113;
	case ENOTEMPTY: return 39;
	case EUSERS: return 87;
	case EDQUOT: return 122;
	case ESTALE: return 116;
	case ENOLCK: return 37;
	case ENOSYS: return 38;
	case EOVERFLOW: return 75;
	case ECANCELED: return 125;
	case EILSEQ: return 84;
	case ETIME: return 62;
	case ENOTRECOVERABLE: return 131;
	case EOWNERDEAD: return 130;
	default:
		/* 1 to 34 agree */
		return value > 0 && value <= 34 ? value : 5; /* EIO */
	}
}

int host_errno(void)
{
	return host_linux_errno(errno);
}

static long result_of(long value)
{
	return value == -1 ? -host_linux_errno(errno) : value;
}

/* ---------- constants */

#define LINUX_AT_FDCWD -100
#define LINUX_AT_SYMLINK_NOFOLLOW 0x100
#define LINUX_AT_REMOVEDIR 0x200
#define LINUX_AT_EACCESS 0x200
#define LINUX_AT_EMPTY_PATH 0x1000

#define LINUX_O_CREAT 0100
#define LINUX_O_EXCL 0200
#define LINUX_O_NOCTTY 0400
#define LINUX_O_TRUNC 01000
#define LINUX_O_APPEND 02000
#define LINUX_O_NONBLOCK 04000
#define LINUX_O_SYNC 04010000
#define LINUX_O_DSYNC 010000
#define LINUX_O_DIRECTORY 040000
#define LINUX_O_NOFOLLOW 0100000
#define LINUX_O_CLOEXEC 02000000

static int directory_descriptor(long value)
{
	return (int)value == LINUX_AT_FDCWD ? AT_FDCWD : (int)value;
}

static int open_flags_in(int flags)
{
	int result = flags & 3;

	if (flags & LINUX_O_CREAT) result |= O_CREAT;
	if (flags & LINUX_O_EXCL) result |= O_EXCL;
	if (flags & LINUX_O_NOCTTY) result |= O_NOCTTY;
	if (flags & LINUX_O_TRUNC) result |= O_TRUNC;
	if (flags & LINUX_O_APPEND) result |= O_APPEND;
	if (flags & LINUX_O_NONBLOCK) result |= O_NONBLOCK;
	if ((flags & LINUX_O_SYNC) == LINUX_O_SYNC) result |= O_SYNC;
	else if (flags & LINUX_O_DSYNC) result |= O_DSYNC;
	if (flags & LINUX_O_DIRECTORY) result |= O_DIRECTORY;
	if (flags & LINUX_O_NOFOLLOW) result |= O_NOFOLLOW;
	if (flags & LINUX_O_CLOEXEC) result |= O_CLOEXEC;
	return result;
}

static int open_flags_out(int flags)
{
	int result = flags & 3;

	if (flags & O_APPEND) result |= LINUX_O_APPEND;
	if (flags & O_NONBLOCK) result |= LINUX_O_NONBLOCK;
	if (flags & O_SYNC) result |= LINUX_O_SYNC;
	return result;
}

/* ---------- structures */

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

/* the kernel's struct stat on AArch64 and the other asm-generic
architectures (arch/x32/kstat.h) */
struct linux_stat
{
	uint64_t st_dev;
	uint64_t st_ino;
	uint32_t st_mode;
	uint32_t st_nlink;
	uint32_t st_uid;
	uint32_t st_gid;
	uint64_t st_rdev;
	uint64_t pad;
	int64_t st_size;
	int32_t st_blksize;
	int32_t pad2;
	int64_t st_blocks;
	int64_t st_atime_sec;
	int64_t st_atime_nsec;
	int64_t st_mtime_sec;
	int64_t st_mtime_nsec;
	int64_t st_ctime_sec;
	int64_t st_ctime_nsec;
	uint32_t unused[2];
};

struct linux_dirent64
{
	uint64_t d_ino;
	int64_t d_off;
	uint16_t d_reclen;
	uint8_t d_type;
	char d_name[];
};

static int timespec_in(uint64_t address, struct timespec *result)
{
	const struct guest_timespec *value = GUEST(const struct guest_timespec *, address);

	if (!(uint32_t)address)
		return 0;
	result->tv_sec = value->seconds;
	result->tv_nsec = value->nanoseconds;
	return 1;
}

static void timespec_out(uint64_t address, const struct timespec *value)
{
	struct guest_timespec *result = GUEST(struct guest_timespec *, address);

	if (!(uint32_t)address)
		return;
	result->seconds = (int32_t)value->tv_sec;
	result->nanoseconds = (int32_t)value->tv_nsec;
}

static void stat_out(uint64_t address, const struct stat *value)
{
	struct linux_stat *result = GUEST(struct linux_stat *, address);

	memset(result, 0, sizeof(*result));
	result->st_dev = (uint64_t)value->st_dev;
	result->st_ino = value->st_ino;
	result->st_mode = value->st_mode;
	result->st_nlink = value->st_nlink;
	result->st_uid = value->st_uid;
	result->st_gid = value->st_gid;
	result->st_rdev = (uint64_t)value->st_rdev;
	result->st_size = value->st_size;
	result->st_blksize = value->st_blksize;
	result->st_blocks = value->st_blocks;
	result->st_atime_sec = value->st_atimespec.tv_sec;
	result->st_atime_nsec = value->st_atimespec.tv_nsec;
	result->st_mtime_sec = value->st_mtimespec.tv_sec;
	result->st_mtime_nsec = value->st_mtimespec.tv_nsec;
	result->st_ctime_sec = value->st_ctimespec.tv_sec;
	result->st_ctime_nsec = value->st_ctimespec.tv_nsec;
}

static clockid_t clock_in(long value)
{
	switch (value)
	{
	case 0: case 5: case 8: return CLOCK_REALTIME;
	case 2: return CLOCK_PROCESS_CPUTIME_ID;
	case 3: return CLOCK_THREAD_CPUTIME_ID;
	case 4: return CLOCK_MONOTONIC_RAW;
	default: return CLOCK_MONOTONIC;
	}
}

/* ---------- vectored reads and writes */

static long guest_writev(int fd, uint64_t vector, int count, int64_t offset, int positional)
{
	struct iovec host_vector[64];
	const struct guest_iovec *guest_vector = GUEST(const struct guest_iovec *, vector);
	int index;

	if (count < 0 || count > 64)
		return -22;
	for (index = 0; index < count; index++)
	{
		host_vector[index].iov_base = GUEST(void *, guest_vector[index].base);
		host_vector[index].iov_len = guest_vector[index].length;
	}
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
		return -22;
	for (index = 0; index < count; index++)
	{
		host_vector[index].iov_base = GUEST(void *, guest_vector[index].base);
		host_vector[index].iov_len = guest_vector[index].length;
	}
	if (positional)
		return result_of(preadv(fd, host_vector, count, offset));
	return result_of(readv(fd, host_vector, count));
}

/* ---------- directories

musl's readdir reads a directory descriptor with getdents64. Each
descriptor read that way gets a DIR (on a duplicate of it), kept until the
descriptor is closed. */

#define DIRECTORY_SLOTS 1024

static DIR *directories[DIRECTORY_SLOTS];
static pthread_mutex_t directories_lock = PTHREAD_MUTEX_INITIALIZER;

static long guest_getdents64(int fd, uint64_t buffer, uint32_t size)
{
	char *out = GUEST(char *, buffer);
	uint32_t used = 0;
	DIR *directory;

	if (fd < 0 || fd >= DIRECTORY_SLOTS)
		return -9; /* EBADF */
	pthread_mutex_lock(&directories_lock);
	directory = directories[fd];
	if (!directory)
	{
		int copy = dup(fd);

		directory = copy >= 0 ? fdopendir(copy) : NULL;
		if (!directory)
		{
			int error = errno;

			if (copy >= 0)
				close(copy);
			pthread_mutex_unlock(&directories_lock);
			return -host_linux_errno(error);
		}
		directories[fd] = directory;
	}
	for (;;)
	{
		long position = telldir(directory);
		struct dirent *entry = readdir(directory);
		struct linux_dirent64 *record;
		size_t length, record_size;

		if (!entry)
			break;
		length = strlen(entry->d_name);
		record_size = (offsetof(struct linux_dirent64, d_name) + length + 1 + 7) & ~(size_t)7;
		if (used + record_size > size)
		{
			seekdir(directory, position);
			if (!used)
			{
				pthread_mutex_unlock(&directories_lock);
				return -22;
			}
			break;
		}
		record = (struct linux_dirent64 *)(out + used);
		record->d_ino = entry->d_ino;
		record->d_off = telldir(directory);
		record->d_reclen = (uint16_t)record_size;
		record->d_type = entry->d_type;
		memcpy(record->d_name, entry->d_name, length + 1);
		used += (uint32_t)record_size;
	}
	pthread_mutex_unlock(&directories_lock);
	return used;
}

/* a seek on a descriptor being read as a directory: musl's rewinddir
(offset 0) and seekdir (an earlier d_off, which is macOS's telldir) */
static int directory_seek(int fd, int64_t offset, int whence)
{
	int handled = 0;

	if (fd < 0 || fd >= DIRECTORY_SLOTS || whence != SEEK_SET)
		return 0;
	pthread_mutex_lock(&directories_lock);
	if (directories[fd])
	{
		if (offset == 0)
			rewinddir(directories[fd]);
		else
			seekdir(directories[fd], (long)offset);
		handled = 1;
	}
	pthread_mutex_unlock(&directories_lock);
	return handled;
}

static void forget_directory(int fd)
{
	if (fd < 0 || fd >= DIRECTORY_SLOTS)
		return;
	pthread_mutex_lock(&directories_lock);
	if (directories[fd])
	{
		closedir(directories[fd]);
		directories[fd] = NULL;
	}
	pthread_mutex_unlock(&directories_lock);
}

/* ---------- futexes (musl's locks, condition variables and joins) */

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_REQUEUE 3
#define FUTEX_CMP_REQUEUE 4
#define FUTEX_WAIT_BITSET 9
#define FUTEX_WAKE_BITSET 10
#define FUTEX_CLOCK_REALTIME 256
#define FUTEX_COMMAND_MASK 127

static long futex_wait(uint32_t *address, uint32_t value, const struct timespec *timeout)
{
	int result;

	if (timeout)
	{
		uint64_t nanoseconds;

		if (timeout->tv_sec < 0)
			return -110; /* ETIMEDOUT */
		nanoseconds = (uint64_t)timeout->tv_sec * 1000000000ull + (uint64_t)timeout->tv_nsec;
		if (!nanoseconds)
			return __atomic_load_n(address, __ATOMIC_SEQ_CST) == value ? -110 : -11;
		result = os_sync_wait_on_address_with_timeout(address, value, sizeof(*address), OS_SYNC_WAIT_ON_ADDRESS_NONE,
			OS_CLOCK_MACH_ABSOLUTE_TIME, nanoseconds);
	}
	else
	{
		result = os_sync_wait_on_address(address, value, sizeof(*address), OS_SYNC_WAIT_ON_ADDRESS_NONE);
	}
	if (result < 0)
	{
		switch (errno)
		{
		case ETIMEDOUT: return -110;
		case EINTR: return -4;
		case EFAULT: return -14;
		case EINVAL: return -22;
		default: return -11;
		}
	}
	return 0;
}

static long futex_wake(uint32_t *address, uint32_t count)
{
	if (count == 0)
		return 0;
	if (count == 1)
		os_sync_wake_by_address_any(address, sizeof(*address), OS_SYNC_WAKE_BY_ADDRESS_NONE);
	else
		os_sync_wake_by_address_all(address, sizeof(*address), OS_SYNC_WAKE_BY_ADDRESS_NONE);
	/* the number woken is unknown; musl only compares it with zero */
	return 1;
}

static long guest_futex(uint64_t address, int operation, uint32_t value, uint64_t timeout, uint64_t address2,
	uint32_t value3)
{
	uint32_t *word = GUEST(uint32_t *, address);
	struct timespec host_timeout;
	int command = operation & FUTEX_COMMAND_MASK;

	(void)value3;
	switch (command)
	{
	case FUTEX_WAIT:
		return futex_wait(word, value, timespec_in(timeout, &host_timeout) ? &host_timeout : NULL);
	case FUTEX_WAIT_BITSET:
	{
		/* an absolute time */
		struct timespec now;

		if (!timespec_in(timeout, &host_timeout))
			return futex_wait(word, value, NULL);
		clock_gettime((operation & FUTEX_CLOCK_REALTIME) ? CLOCK_REALTIME : CLOCK_MONOTONIC, &now);
		host_timeout.tv_sec -= now.tv_sec;
		host_timeout.tv_nsec -= now.tv_nsec;
		if (host_timeout.tv_nsec < 0)
		{
			host_timeout.tv_nsec += 1000000000L;
			host_timeout.tv_sec--;
		}
		return futex_wait(word, value, &host_timeout);
	}
	case FUTEX_WAKE:
	case FUTEX_WAKE_BITSET:
		return futex_wake(word, value);
	case FUTEX_REQUEUE:
	case FUTEX_CMP_REQUEUE:
		/* waking every waiter instead of moving them is allowed: each
		waiter checks its condition again and waits once more */
		futex_wake(word, 0x7fffffff);
		if (address2)
			futex_wake(GUEST(uint32_t *, address2), 0x7fffffff);
		return 1;
	default:
		return -38;
	}
}

/* ---------- process */

static long thread_id(void)
{
	uint64_t id = 0;

	pthread_threadid_np(NULL, &id);
	return (long)(id & 0x3fffffff);
}

static void raise_signal(long signal_number)
{
	/* the guest only raises SIGABRT (abort) */
	if (signal_number == 6)
	{
		host_logf(HOST_LOG_ERROR, "the game aborted");
		abort();
	}
}

struct linux_utsname
{
	char sysname[65];
	char nodename[65];
	char release[65];
	char version[65];
	char machine[65];
	char domainname[65];
};

static long guest_uname(uint64_t address)
{
	struct linux_utsname *result = GUEST(struct linux_utsname *, address);

	memset(result, 0, sizeof(*result));
	strcpy(result->sysname, "Darwin");
	gethostname(result->nodename, sizeof(result->nodename) - 1);
	strcpy(result->release, "halo-macos");
	strcpy(result->version, "1");
	strcpy(result->machine, "x86_64");
	return 0;
}

static long guest_readlinkat(long directory, uint64_t path, uint64_t buffer, uint32_t size)
{
	const char *name = GUEST(const char *, path);

	if (!strcmp(name, "/proc/self/exe"))
	{
		size_t length = strlen(host_executable_path);

		if (length > size)
			length = size;
		memcpy(GUEST(char *, buffer), host_executable_path, length);
		return (long)length;
	}
	return result_of(readlinkat(directory_descriptor(directory), name, GUEST(char *, buffer), size));
}

static long guest_fcntl(int fd, int command, long argument)
{
	switch (command)
	{
	case 0: /* F_DUPFD */
		return result_of(fcntl(fd, F_DUPFD, (int)argument));
	case 1030: /* F_DUPFD_CLOEXEC */
		return result_of(fcntl(fd, F_DUPFD_CLOEXEC, (int)argument));
	case 1: /* F_GETFD */
		return result_of(fcntl(fd, F_GETFD));
	case 2: /* F_SETFD */
		return result_of(fcntl(fd, F_SETFD, (int)argument));
	case 3: /* F_GETFL */
	{
		long flags = result_of(fcntl(fd, F_GETFL));

		return flags < 0 ? flags : open_flags_out((int)flags);
	}
	case 4: /* F_SETFL */
		return result_of(fcntl(fd, F_SETFL, open_flags_in((int)argument) & (O_APPEND | O_NONBLOCK)));
	case 5: case 6: case 7: /* F_GETLK, F_SETLK, F_SETLKW: record locks are not used */
		return 0;
	default:
		return -22;
	}
}

static long guest_prlimit(int resource, uint64_t old_limit)
{
	uint64_t *result = GUEST(uint64_t *, old_limit);

	if (!(uint32_t)old_limit)
		return 0;
	result[0] = result[1] = ~0ULL;
	if (resource == 7) /* RLIMIT_NOFILE */
	{
		struct rlimit limit;

		if (getrlimit(RLIMIT_NOFILE, &limit) == 0)
		{
			result[0] = limit.rlim_cur;
			result[1] = limit.rlim_max;
		}
	}
	else if (resource == 3) /* RLIMIT_STACK */
	{
		result[0] = result[1] = 16 * 1024 * 1024;
	}
	return 0;
}

static long guest_utimensat(long directory, uint64_t path, uint64_t times_address, int flags)
{
	const struct guest_timespec *guest_times = (uint32_t)times_address ?
		GUEST(const struct guest_timespec *, times_address) : NULL;
	struct timespec times[2];
	int index;

	for (index = 0; guest_times && index < 2; index++)
	{
		times[index].tv_sec = guest_times[index].seconds;
		times[index].tv_nsec = guest_times[index].nanoseconds;
		if (guest_times[index].nanoseconds == (1 << 30) - 1)
			times[index].tv_nsec = UTIME_NOW;
		else if (guest_times[index].nanoseconds == (1 << 30) - 2)
			times[index].tv_nsec = UTIME_OMIT;
	}
	if (!(uint32_t)path)
		return result_of(futimens((int)directory, guest_times ? times : NULL));
	return result_of(utimensat(directory_descriptor(directory), GUEST(const char *, path), guest_times ? times : NULL,
		(flags & LINUX_AT_SYMLINK_NOFOLLOW) ? AT_SYMLINK_NOFOLLOW : 0));
}

/* ---------- dispatch */

long long host_syscall(long long number, long long a, long long b, long long c,
	long long d, long long e, long long f)
{
	switch (number)
	{
	case __NR_read:
		return result_of(read((int)a, GUEST(void *, b), (size_t)(uint32_t)c));
	case __NR_write:
		return result_of(write((int)a, GUEST(const void *, b), (size_t)(uint32_t)c));
	case __NR_pread64:
		return result_of(pread((int)a, GUEST(void *, b), (size_t)(uint32_t)c, d));
	case __NR_pwrite64:
		return result_of(pwrite((int)a, GUEST(const void *, b), (size_t)(uint32_t)c, d));
	case __NR_writev:
		return guest_writev((int)a, (uint64_t)b, (int)c, 0, 0);
	case __NR_pwritev:
		return guest_writev((int)a, (uint64_t)b, (int)c, d, 1);
	case __NR_readv:
		return guest_readv((int)a, (uint64_t)b, (int)c, 0, 0);
	case __NR_preadv:
		return guest_readv((int)a, (uint64_t)b, (int)c, d, 1);
	case __NR_openat:
		return result_of(openat(directory_descriptor(a), GUEST(const char *, b), open_flags_in((int)c), (int)d));
	case __NR_close:
		forget_directory((int)a);
		return result_of(close((int)a));
	case __NR_lseek:
		if (directory_seek((int)a, (int64_t)b, (int)c))
			return b;
		return result_of((long)lseek((int)a, (off_t)b, (int)c));
	case __NR_getdents64:
		return guest_getdents64((int)a, (uint64_t)b, (uint32_t)c);
	case __NR_fstat:
	{
		struct stat information;
		long result = result_of(fstat((int)a, &information));

		if (result == 0)
			stat_out((uint64_t)b, &information);
		return result;
	}
	case __NR_newfstatat:
	{
		struct stat information;
		const char *path = (uint32_t)b ? GUEST(const char *, b) : NULL;
		long result;

		if ((d & LINUX_AT_EMPTY_PATH) && (!path || !*path))
			result = result_of(fstat((int)a, &information));
		else
			result = result_of(fstatat(directory_descriptor(a), path, &information,
				(d & LINUX_AT_SYMLINK_NOFOLLOW) ? AT_SYMLINK_NOFOLLOW : 0));
		if (result == 0)
			stat_out((uint64_t)c, &information);
		return result;
	}
	case __NR_unlinkat:
		return result_of(unlinkat(directory_descriptor(a), GUEST(const char *, b),
			(c & LINUX_AT_REMOVEDIR) ? AT_REMOVEDIR : 0));
	case __NR_renameat:
		return result_of(renameat(directory_descriptor(a), GUEST(const char *, b), directory_descriptor(c),
			GUEST(const char *, d)));
	case __NR_renameat2:
		if (e)
			return -22;
		return result_of(renameat(directory_descriptor(a), GUEST(const char *, b), directory_descriptor(c),
			GUEST(const char *, d)));
	case __NR_mkdirat:
		return result_of(mkdirat(directory_descriptor(a), GUEST(const char *, b), (mode_t)c));
	case __NR_fchmod:
		return result_of(fchmod((int)a, (mode_t)b));
	case __NR_fchmodat:
		return result_of(fchmodat(directory_descriptor(a), GUEST(const char *, b), (mode_t)c, 0));
	case __NR_faccessat:
		return result_of(faccessat(directory_descriptor(a), GUEST(const char *, b), (int)c,
			(d & LINUX_AT_EACCESS) ? AT_EACCESS : 0));
	case __NR_readlinkat:
		return guest_readlinkat(a, (uint64_t)b, (uint64_t)c, (uint32_t)d);
	case __NR_ftruncate:
		return result_of(ftruncate((int)a, (off_t)b));
	case __NR_fsync:
	case __NR_fdatasync:
		return result_of(fsync((int)a));
	case __NR_fcntl:
		return guest_fcntl((int)a, (int)b, (long)c);
	case __NR_getcwd:
	{
		char *buffer = GUEST(char *, a);

		if (!getcwd(buffer, (size_t)(uint32_t)b))
			return -host_linux_errno(errno);
		return (long)strlen(buffer) + 1;
	}
	case __NR_chdir:
		return result_of(chdir(GUEST(const char *, a)));
	case __NR_dup:
		return result_of(dup((int)a));
	case __NR_dup3:
	{
		long result;

		forget_directory((int)b);
		result = result_of(dup2((int)a, (int)b));

		if (result >= 0 && (c & LINUX_O_CLOEXEC))
			fcntl((int)result, F_SETFD, FD_CLOEXEC);
		return result;
	}
	case __NR_pipe2:
	{
		int descriptors[2];
		int32_t *result = GUEST(int32_t *, a);
		int index;

		if (pipe(descriptors) != 0)
			return -host_linux_errno(errno);
		for (index = 0; index < 2; index++)
		{
			if (b & LINUX_O_CLOEXEC)
				fcntl(descriptors[index], F_SETFD, FD_CLOEXEC);
			if (b & LINUX_O_NONBLOCK)
				fcntl(descriptors[index], F_SETFL, O_NONBLOCK);
			result[index] = descriptors[index];
		}
		return 0;
	}
	case __NR_flock:
		return result_of(flock((int)a, (int)b));
	case __NR_umask:
		return (long)umask((mode_t)a);
	case __NR_utimensat:
		return guest_utimensat(a, (uint64_t)b, (uint64_t)c, (int)d);
	case __NR_ioctl:
		return -25; /* ENOTTY */

	case __NR_clock_gettime:
	case __NR_clock_getres:
	{
		struct timespec value;
		long result = number == __NR_clock_gettime ? clock_gettime(clock_in(a), &value) :
			clock_getres(clock_in(a), &value);

		if (result == 0)
			timespec_out((uint64_t)b, &value);
		return result_of(result);
	}
	case __NR_gettimeofday:
	{
		struct timespec value;
		struct guest_timespec *result = (uint32_t)a ? GUEST(struct guest_timespec *, a) : NULL;

		clock_gettime(CLOCK_REALTIME, &value);
		if (result)
		{
			result->seconds = (int32_t)value.tv_sec;
			result->nanoseconds = (int32_t)(value.tv_nsec / 1000);
		}
		return 0;
	}
	case __NR_nanosleep:
	{
		struct timespec request, remaining;
		long result;

		if (!timespec_in((uint64_t)a, &request))
			return -14;
		result = result_of(nanosleep(&request, &remaining));
		if (result == -4)
			timespec_out((uint64_t)b, &remaining);
		return result;
	}
	case __NR_clock_nanosleep:
	{
		struct timespec request, remaining;
		long result;

		if (!timespec_in((uint64_t)c, &request))
			return -14;
		if (b & 1) /* TIMER_ABSTIME */
		{
			struct timespec now;

			clock_gettime(clock_in(a), &now);
			request.tv_sec -= now.tv_sec;
			request.tv_nsec -= now.tv_nsec;
			if (request.tv_nsec < 0)
			{
				request.tv_nsec += 1000000000L;
				request.tv_sec--;
			}
			if (request.tv_sec < 0)
				return 0;
		}
		result = result_of(nanosleep(&request, &remaining));
		if (result == -4 && !(b & 1))
			timespec_out((uint64_t)d, &remaining);
		return result;
	}
	case __NR_futex:
		return guest_futex((uint64_t)a, (int)b, (uint32_t)c, (uint64_t)d, (uint64_t)e, (uint32_t)f);
	case __NR_ppoll:
	{
		struct timespec timeout;
		int milliseconds = -1;

		if (timespec_in((uint64_t)c, &timeout))
		{
			long long total = (long long)timeout.tv_sec * 1000 + timeout.tv_nsec / 1000000;

			milliseconds = total < 0 ? 0 : total > 0x7fffffff ? 0x7fffffff : (int)total;
		}
		return result_of(poll(GUEST(struct pollfd *, a), (nfds_t)(uint32_t)b, milliseconds));
	}
	case __NR_sched_yield:
		return result_of(sched_yield());
	case __NR_sched_getaffinity:
	{
		/* one bit per processor, for sysconf(_SC_NPROCESSORS_ONLN) */
		unsigned char *mask = GUEST(unsigned char *, c);
		int count = 0, index;
		size_t length = sizeof(count);

		sysctlbyname("hw.logicalcpu", &count, &length, NULL, 0);
		if (count < 1)
			count = 1;
		if ((uint32_t)b < 8)
			return -22;
		memset(mask, 0, (uint32_t)b);
		for (index = 0; index < count && index < (int)(uint32_t)b * 8; index++)
			mask[index / 8] |= (unsigned char)(1 << (index % 8));
		return 8;
	}

	case __NR_mmap:
		return host_guest_mmap((uint64_t)a, (uint64_t)b, (int)c, (int)d, (int)e, f);
	case __NR_munmap:
		return host_guest_munmap((uint64_t)a, (uint64_t)b);
	case __NR_mprotect:
		return host_guest_mprotect((uint64_t)a, (uint64_t)b, (int)c);
	case __NR_madvise:
		return 0;
	case __NR_mremap:
	case __NR_brk:
		/* musl then falls back to mmap and copying */
		return -12;

	case __NR_exit:
	case __NR_exit_group:
		host_exit((int)a);

	case __NR_set_tid_address:
	case __NR_gettid:
		return thread_id();
	case __NR_getpid:
		return getpid();
	case __NR_getppid:
		return getppid();
	case __NR_getuid:
		return getuid();
	case __NR_geteuid:
		return geteuid();
	case __NR_getgid:
		return getgid();
	case __NR_getegid:
		return getegid();
	case __NR_kill:
		raise_signal(b);
		return 0;
	case __NR_tkill:
		raise_signal(b);
		return 0;
	case __NR_tgkill:
		raise_signal(c);
		return 0;
	case __NR_rt_sigprocmask:
		if ((uint32_t)c)
			memset(GUEST(void *, c), 0, (size_t)(uint32_t)d);
		return 0;
	case __NR_rt_sigaction:
	case __NR_sigaltstack:
		/* the host owns signal handling */
		return 0;
	case __NR_uname:
		return guest_uname((uint64_t)a);
	case __NR_prlimit64:
		return guest_prlimit((int)b, (uint64_t)d);
	case __NR_getrlimit:
		return -38;
	case __NR_getrandom:
		arc4random_buf(GUEST(void *, a), (size_t)(uint32_t)b);
		return (uint32_t)b;
	case __NR_membarrier:
	case __NR_statx:
	case __NR_statfs:
	case __NR_fstatfs:
	case __NR_sysinfo:
		return -38; /* ENOSYS */

	default:
		host_logf(HOST_LOG_WARN, "guest system call %lld is not supported", number);
		return -38;
	}
}
