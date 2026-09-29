#include "host_services.h"

#include "guest_address.h"
#include "guest_call.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Linux i386 open(2) flags from the guest ABI. */
#define GUEST_O_ACCMODE   00000003u
#define GUEST_O_CREAT     00000100u
#define GUEST_O_EXCL      00000200u
#define GUEST_O_NOCTTY    00000400u
#define GUEST_O_TRUNC     00001000u
#define GUEST_O_APPEND    00002000u
#define GUEST_O_NONBLOCK  00004000u
#define GUEST_O_ASYNC     00020000u
#define GUEST_O_DIRECT    00040000u
#define GUEST_O_LARGEFILE 00100000u
#define GUEST_O_DIRECTORY 00200000u
#define GUEST_O_NOFOLLOW  00400000u
#define GUEST_O_NOATIME   01000000u
#define GUEST_O_CLOEXEC   02000000u
#define GUEST_O_DSYNC     00010000u
#define GUEST_O_SYNC      04010000u
#define GUEST_AT_FDCWD    (-100)

enum guest_linux_errno
{
	_guest_eperm = 1, _guest_enoent = 2, _guest_esrch = 3, _guest_eintr = 4,
	_guest_eio = 5, _guest_enxio = 6, _guest_e2big = 7, _guest_enoexec = 8,
	_guest_ebadf = 9, _guest_echild = 10, _guest_eagain = 11, _guest_enomem = 12,
	_guest_eacces = 13, _guest_efault = 14, _guest_ebusy = 16, _guest_eexist = 17,
	_guest_exdev = 18, _guest_enodev = 19, _guest_enotdir = 20, _guest_eisdir = 21,
	_guest_einval = 22, _guest_enfile = 23, _guest_emfile = 24, _guest_enotty = 25,
	_guest_etxtbsy = 26, _guest_efbig = 27, _guest_enospc = 28, _guest_espipe = 29,
	_guest_erofs = 30, _guest_emlink = 31, _guest_epipe = 32, _guest_edom = 33,
	_guest_erange = 34, _guest_edeadlk = 35, _guest_enametoolong = 36,
	_guest_enolck = 37, _guest_enosys = 38, _guest_enotempty = 39, _guest_eloop = 40,
	_guest_eoverflow = 75,
	_guest_enomsg = 42, _guest_eidrm = 43, _guest_enostr = 60, _guest_enodata = 61,
	_guest_etime = 62, _guest_enosr = 63, _guest_enotsock = 88, _guest_edestaddrreq = 89,
	_guest_emsgsize = 90, _guest_eprototype = 91, _guest_enoprotoopt = 92,
	_guest_eprotonosupport = 93, _guest_esocktnosupport = 94, _guest_eopnotsupp = 95,
	_guest_epfnosupport = 96, _guest_eafnosupport = 97, _guest_eaddrinuse = 98,
	_guest_eaddrnotavail = 99, _guest_enetdown = 100, _guest_enetunreach = 101,
	_guest_enetreset = 102, _guest_econnaborted = 103, _guest_econnreset = 104,
	_guest_enobufs = 105, _guest_eisconn = 106, _guest_enotconn = 107,
	_guest_eshutdown = 108, _guest_etoomanyrefs = 109, _guest_etimedout = 110,
	_guest_econnrefused = 111, _guest_ehostdown = 112, _guest_ehostunreach = 113,
	_guest_ealready = 114, _guest_einprogress = 115, _guest_estale = 116,
	_guest_ecanceled = 125, _guest_eownerdead = 130, _guest_enotrecoverable = 131
};

struct guest_timespec32
{
	int32_t seconds;
	int32_t nanoseconds;
};

/* Linux clock_gettime64's kernel record, independent of host long/time_t. */
struct guest_kernel_timespec64
{
	int64_t seconds;
	int64_t nanoseconds;
};
_Static_assert(sizeof(struct guest_kernel_timespec64) == 16,
	"guest time64 clock record must contain two 64-bit fields");

struct guest_timeval32
{
	int32_t seconds;
	int32_t microseconds;
};

static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local uint32_t guest_tp;
static _Thread_local int config_trace_fd = -1;

void mac_host_log(int priority, const char *message)
{
	static const char *const levels[] = { "debug", "info", "warn", "error", "fatal" };
	const char *level = priority < 0 || priority >= (int)(sizeof(levels) / sizeof(levels[0])) ?
		"info" : levels[priority];

	pthread_mutex_lock(&log_mutex);
	fprintf(stderr, "halo-macos[%s]: %s\n", level, message ? message : "(null)");
	fflush(stderr);
	pthread_mutex_unlock(&log_mutex);
}

void mac_host_logf(int priority, const char *format, ...)
{
	char message[2048];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);
	mac_host_log(priority, message);
}

void mac_host_abort(const char *message)
{
	mac_host_log(4, message ? message : "guest abort");
	abort();
}

void mac_host_exit(int status)
{
	mac_host_logf(1, "guest process exit (%d)", status);
	exit(status);
}

int mac_host_errno(void)
{
	return mac_host_linux_errno(errno);
}

int mac_host_linux_errno(int darwin_errno)
{
	switch (darwin_errno)
	{
	case EPERM: return _guest_eperm;
	case ENOENT: return _guest_enoent;
	case ESRCH: return _guest_esrch;
	case EINTR: return _guest_eintr;
	case EIO: return _guest_eio;
	case ENXIO: return _guest_enxio;
	case E2BIG: return _guest_e2big;
	case ENOEXEC: return _guest_enoexec;
	case EBADF: return _guest_ebadf;
	case ECHILD: return _guest_echild;
	case EAGAIN: return _guest_eagain;
	case ENOMEM: return _guest_enomem;
	case EACCES: return _guest_eacces;
	case EFAULT: return _guest_efault;
	case EBUSY: return _guest_ebusy;
	case EEXIST: return _guest_eexist;
	case EXDEV: return _guest_exdev;
	case ENODEV: return _guest_enodev;
	case ENOTDIR: return _guest_enotdir;
	case EISDIR: return _guest_eisdir;
	case EINVAL: return _guest_einval;
	case ENFILE: return _guest_enfile;
	case EMFILE: return _guest_emfile;
	case ENOTTY: return _guest_enotty;
	case ETXTBSY: return _guest_etxtbsy;
	case EFBIG: return _guest_efbig;
	case ENOSPC: return _guest_enospc;
	case ESPIPE: return _guest_espipe;
	case EROFS: return _guest_erofs;
	case EMLINK: return _guest_emlink;
	case EPIPE: return _guest_epipe;
	case EDOM: return _guest_edom;
	case ERANGE: return _guest_erange;
	case EDEADLK: return _guest_edeadlk;
	case ENAMETOOLONG: return _guest_enametoolong;
	case ENOLCK: return _guest_enolck;
	case ENOSYS: return _guest_enosys;
	case ENOTEMPTY: return _guest_enotempty;
	case ELOOP: return _guest_eloop;
#ifdef ENOMSG
	case ENOMSG: return _guest_enomsg;
#endif
#ifdef EIDRM
	case EIDRM: return _guest_eidrm;
#endif
#ifdef ENOSTR
	case ENOSTR: return _guest_enostr;
#endif
#ifdef ENODATA
	case ENODATA: return _guest_enodata;
#endif
#ifdef ETIME
	case ETIME: return _guest_etime;
#endif
#ifdef ENOSR
	case ENOSR: return _guest_enosr;
#endif
	case ENOTSOCK: return _guest_enotsock;
	case EDESTADDRREQ: return _guest_edestaddrreq;
	case EMSGSIZE: return _guest_emsgsize;
	case EPROTOTYPE: return _guest_eprototype;
	case ENOPROTOOPT: return _guest_enoprotoopt;
	case EPROTONOSUPPORT: return _guest_eprotonosupport;
#ifdef ESOCKTNOSUPPORT
	case ESOCKTNOSUPPORT: return _guest_esocktnosupport;
#endif
	case EOPNOTSUPP: return _guest_eopnotsupp;
#ifdef EPFNOSUPPORT
	case EPFNOSUPPORT: return _guest_epfnosupport;
#endif
	case EAFNOSUPPORT: return _guest_eafnosupport;
	case EADDRINUSE: return _guest_eaddrinuse;
	case EADDRNOTAVAIL: return _guest_eaddrnotavail;
	case ENETDOWN: return _guest_enetdown;
	case ENETUNREACH: return _guest_enetunreach;
	case ENETRESET: return _guest_enetreset;
	case ECONNABORTED: return _guest_econnaborted;
	case ECONNRESET: return _guest_econnreset;
	case ENOBUFS: return _guest_enobufs;
	case EISCONN: return _guest_eisconn;
	case ENOTCONN: return _guest_enotconn;
	case ESHUTDOWN: return _guest_eshutdown;
#ifdef ETOOMANYREFS
	case ETOOMANYREFS: return _guest_etoomanyrefs;
#endif
	case ETIMEDOUT: return _guest_etimedout;
	case ECONNREFUSED: return _guest_econnrefused;
#ifdef EHOSTDOWN
	case EHOSTDOWN: return _guest_ehostdown;
#endif
	case EHOSTUNREACH: return _guest_ehostunreach;
	case EALREADY: return _guest_ealready;
	case EINPROGRESS: return _guest_einprogress;
#ifdef ESTALE
	case ESTALE: return _guest_estale;
#endif
#ifdef ECANCELED
	case ECANCELED: return _guest_ecanceled;
#endif
#ifdef EOWNERDEAD
	case EOWNERDEAD: return _guest_eownerdead;
#endif
#ifdef ENOTRECOVERABLE
	case ENOTRECOVERABLE: return _guest_enotrecoverable;
#endif
	default:
		mac_host_logf(3, "unmapped Darwin errno %d; reporting Linux EIO", darwin_errno);
		return _guest_eio;
	}
}

uint64_t mac_host_thread_id(void)
{
	uint64_t thread_id = 0;
	(void)pthread_threadid_np(NULL, &thread_id);
	return thread_id;
}

uint32_t mac_host_get_guest_tp(void)
{
	return guest_tp;
}

void mac_host_set_guest_tp(uint32_t value)
{
	guest_tp = value;
}

static int host_clock_id(int guest_clock_id, clockid_t *host_clock_id_out)
{
	switch (guest_clock_id)
	{
	case 0: *host_clock_id_out = CLOCK_REALTIME; return 0;
	case 1: *host_clock_id_out = CLOCK_MONOTONIC; return 0;
#ifdef CLOCK_PROCESS_CPUTIME_ID
	case 2: *host_clock_id_out = CLOCK_PROCESS_CPUTIME_ID; return 0;
#endif
#ifdef CLOCK_THREAD_CPUTIME_ID
	case 3: *host_clock_id_out = CLOCK_THREAD_CPUTIME_ID; return 0;
#endif
	default: return -_guest_einval;
	}
}

static int64_t copy_timespec_to_guest(uint32_t address, const struct timespec *value)
{
	struct guest_timespec32 guest_value;

	if (!address)
		return 0;
	if (value->tv_sec < INT32_MIN || value->tv_sec > INT32_MAX ||
		value->tv_nsec < INT32_MIN || value->tv_nsec > INT32_MAX)
		return -_guest_eoverflow;
	guest_value.seconds = (int32_t)value->tv_sec;
	guest_value.nanoseconds = (int32_t)value->tv_nsec;
	return mac_guest_write(address, &guest_value, sizeof(guest_value)) == 0 ? 0 : -_guest_efault;
}

int64_t mac_host_clock_gettime(int guest_clock_id, uint32_t guest_timespec_va)
{
	clockid_t clock_id;
	struct timespec value;

	if (host_clock_id(guest_clock_id, &clock_id) != 0)
		return -_guest_einval;
	if (clock_gettime(clock_id, &value) != 0)
		return -mac_host_linux_errno(errno);
	return copy_timespec_to_guest(guest_timespec_va, &value);
}

int64_t mac_host_clock_getres(int guest_clock_id, uint32_t guest_timespec_va)
{
	clockid_t clock_id;
	struct timespec value;

	if (host_clock_id(guest_clock_id, &clock_id) != 0)
		return -_guest_einval;
	if (clock_getres(clock_id, &value) != 0)
		return -mac_host_linux_errno(errno);
	return copy_timespec_to_guest(guest_timespec_va, &value);
}

int64_t mac_host_clock_gettime64(int guest_clock_id, uint32_t guest_timespec_va)
{
	clockid_t clock_id;
	struct timespec value;
	struct guest_kernel_timespec64 guest_value;

	if (host_clock_id(guest_clock_id, &clock_id) != 0)
		return -_guest_einval;
	if (clock_gettime(clock_id, &value) != 0)
		return -mac_host_linux_errno(errno);
	guest_value.seconds = (int64_t)value.tv_sec;
	guest_value.nanoseconds = (int64_t)value.tv_nsec;
	return mac_guest_write(guest_timespec_va, &guest_value, sizeof(guest_value)) == 0 ?
		0 : -_guest_efault;
}

int64_t mac_host_gettimeofday(uint32_t guest_timeval_va)
{
	struct timeval value;
	struct guest_timeval32 guest_value;

	if (!guest_timeval_va)
		return 0;
	if (gettimeofday(&value, NULL) != 0)
		return -mac_host_linux_errno(errno);
	if (value.tv_sec < INT32_MIN || value.tv_sec > INT32_MAX)
		return -_guest_eoverflow;
	guest_value.seconds = (int32_t)value.tv_sec;
	guest_value.microseconds = (int32_t)value.tv_usec;
	return mac_guest_write(guest_timeval_va, &guest_value, sizeof(guest_value)) == 0 ? 0 : -_guest_efault;
}

static int guest_open_flags(uint32_t guest_flags, int *host_flags_out)
{
	const uint32_t known_flags = GUEST_O_ACCMODE | GUEST_O_CREAT | GUEST_O_EXCL |
		GUEST_O_NOCTTY | GUEST_O_TRUNC | GUEST_O_APPEND | GUEST_O_NONBLOCK |
		GUEST_O_ASYNC | GUEST_O_DIRECT | GUEST_O_LARGEFILE | GUEST_O_DIRECTORY |
		GUEST_O_NOFOLLOW | GUEST_O_NOATIME | GUEST_O_CLOEXEC | GUEST_O_DSYNC |
		GUEST_O_SYNC;
	int flags;

	if ((guest_flags & ~known_flags) || (guest_flags & GUEST_O_ACCMODE) == 3 ||
		(guest_flags & (GUEST_O_DIRECT | GUEST_O_NOATIME)))
		return -_guest_einval;
	switch (guest_flags & GUEST_O_ACCMODE)
	{
	case 0: flags = O_RDONLY; break;
	case 1: flags = O_WRONLY; break;
	default: flags = O_RDWR; break;
	}
#define MAP_OPEN_FLAG(guest_flag, host_flag) do { if (guest_flags & (guest_flag)) flags |= (host_flag); } while (0)
	MAP_OPEN_FLAG(GUEST_O_CREAT, O_CREAT);
	MAP_OPEN_FLAG(GUEST_O_EXCL, O_EXCL);
	MAP_OPEN_FLAG(GUEST_O_NOCTTY, O_NOCTTY);
	MAP_OPEN_FLAG(GUEST_O_TRUNC, O_TRUNC);
	MAP_OPEN_FLAG(GUEST_O_APPEND, O_APPEND);
	MAP_OPEN_FLAG(GUEST_O_NONBLOCK, O_NONBLOCK);
	MAP_OPEN_FLAG(GUEST_O_DIRECTORY, O_DIRECTORY);
	MAP_OPEN_FLAG(GUEST_O_NOFOLLOW, O_NOFOLLOW);
	MAP_OPEN_FLAG(GUEST_O_CLOEXEC, O_CLOEXEC);
#ifdef O_ASYNC
	MAP_OPEN_FLAG(GUEST_O_ASYNC, O_ASYNC);
#else
	if (guest_flags & GUEST_O_ASYNC) return -_guest_einval;
#endif
#ifdef O_DSYNC
	if (guest_flags & GUEST_O_DSYNC) flags |= O_DSYNC;
#else
	if (guest_flags & GUEST_O_DSYNC) return -_guest_einval;
#endif
	if (guest_flags & GUEST_O_SYNC) flags |= O_SYNC;
#undef MAP_OPEN_FLAG
	*host_flags_out = flags;
	return 0;
}

static int guest_path_copy(uint32_t path_va, char path[PATH_MAX])
{
	const char *source;
	size_t available, scan_size, length;

	if (!path_va || !(source = mac_guest_address_resolve(path_va, 1)))
		return -_guest_efault;
	available = mac_guest_address_available(path_va);
	scan_size = available < PATH_MAX ? available : PATH_MAX;
	if (!scan_size)
		return -_guest_efault;
	length = strnlen(source, scan_size);
	if (length == scan_size)
		return -_guest_enametoolong;
	memcpy(path, source, length + 1);
	return 0;
}

int64_t mac_host_openat(int directory_fd, uint32_t guest_path_va,
	uint32_t linux_flags, uint32_t mode)
{
	char path[PATH_MAX];
	int host_flags;
	int result;
	int error = guest_open_flags(linux_flags, &host_flags);

	if (error)
		return error;
	error = guest_path_copy(guest_path_va, path);
	if (error)
		return error;
	result = openat(directory_fd == GUEST_AT_FDCWD ? AT_FDCWD : directory_fd,
		path, host_flags, (mode_t)mode);
	int64_t returned = result < 0 ? -mac_host_linux_errno(errno) : result;
	if (getenv("HALO_TRACE_CONFIG_IO") && strstr(path, "config.toml"))
	{
		config_trace_fd = result;
		mac_host_logf(1, "[config-io] open path=%s flags=0x%x result=%lld",
			path, linux_flags, (long long)returned);
	}
	return returned;
}

static int64_t guest_io_buffer(int fd, uint32_t buffer_va, uint32_t size,
	int64_t offset, int positional, int writing)
{
	void *buffer = NULL;
	ssize_t result;

	if (size)
	{
		buffer = mac_guest_address_resolve(buffer_va, size);
		if (!buffer)
			return -_guest_efault;
	}
	if (size > (uint32_t)SSIZE_MAX)
		return -_guest_einval;
	if (writing)
		result = positional ? pwrite(fd, buffer, size, (off_t)offset) : write(fd, buffer, size);
	else
		result = positional ? pread(fd, buffer, size, (off_t)offset) : read(fd, buffer, size);
	return result < 0 ? -mac_host_linux_errno(errno) : result;
}

int64_t mac_host_read(int fd, uint32_t guest_buffer_va, uint32_t size)
{
	return guest_io_buffer(fd, guest_buffer_va, size, 0, 0, 0);
}

int64_t mac_host_write(int fd, uint32_t guest_buffer_va, uint32_t size)
{
	return guest_io_buffer(fd, guest_buffer_va, size, 0, 0, 1);
}

int64_t mac_host_pread(int fd, uint32_t guest_buffer_va, uint32_t size, int64_t offset)
{
	return guest_io_buffer(fd, guest_buffer_va, size, offset, 1, 0);
}

int64_t mac_host_pwrite(int fd, uint32_t guest_buffer_va, uint32_t size, int64_t offset)
{
	return guest_io_buffer(fd, guest_buffer_va, size, offset, 1, 1);
}

int64_t mac_host_lseek(int fd, int64_t offset, int whence)
{
	off_t result = lseek(fd, (off_t)offset, whence);
	int64_t returned = result == (off_t)-1 ? -mac_host_linux_errno(errno) : (int64_t)result;
	if (getenv("HALO_TRACE_CONFIG_IO") && fd == config_trace_fd)
		mac_host_logf(1, "[config-io] seek fd=%d offset=%lld whence=%d result=%lld",
			fd, (long long)offset, whence, (long long)returned);
	return returned;
}

int64_t mac_host_close(int fd)
{
	return close(fd) == 0 ? 0 : -mac_host_linux_errno(errno);
}
