#include "host_syscall.h"

#include "guest_address.h"
#include "guest_call.h"
#include "host_services.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

/* The ELF guest is i386/ILP32, but its syscall number macros are generated
 * from Android's arm64_32 Linux table.  Its seven cdecl long-long slots carry
 * those numbers unchanged; pointers remain 32-bit. The shared Linux time64
 * extension uses a separate 16-byte record, preserving legacy time32 calls. */
enum arm64_32_syscall_number
{
	_guest_exit = 93, _guest_exit_group = 94, _guest_futex = 98,
	_guest_nanosleep = 101, _guest_clock_gettime = 113, _guest_clock_nanosleep = 115,
	_guest_sched_yield = 124, _guest_gettimeofday = 169,
	_guest_getpid = 172, _guest_getuid = 174, _guest_geteuid = 175,
	_guest_getgid = 176, _guest_getegid = 177, _guest_gettid = 178,
	_guest_munmap = 215, _guest_mmap = 222, _guest_mprotect = 226,
	_guest_openat = 56, _guest_close = 57, _guest_lseek = 62,
	_guest_read = 63, _guest_write = 64, _guest_readv = 65, _guest_writev = 66,
	_guest_pread64 = 67, _guest_pwrite64 = 68, _guest_mkdirat = 34,
	_guest_unlinkat = 35, _guest_chdir = 49, _guest_getrandom = 278,
	/* Linux's time64 extension is shared by i386 and arm64_32. */
	_guest_clock_gettime64 = 403, _guest_clock_nanosleep_time64 = 407
};

enum
{
	_guest_eperm = 1, _guest_enoent = 2, _guest_eintr = 4, _guest_eio = 5,
	_guest_ebadf = 9, _guest_eagain = 11, _guest_enomem = 12,
	_guest_efault = 14, _guest_eexist = 17, _guest_einval = 22,
	_guest_enosys = 38, _guest_etimedout = 110
};

enum
{
	_guest_prot_none = 0, _guest_prot_read = 1, _guest_prot_write = 2,
	_guest_prot_exec = 4,
	_guest_map_shared = 1, _guest_map_private = 2, _guest_map_fixed = 0x10,
	_guest_map_anonymous = 0x20,
	_guest_futex_wait = 0, _guest_futex_wake = 1,
	_guest_futex_private = 128, _guest_futex_realtime = 256
};

struct guest_timespec32
{
	int32_t seconds;
	int32_t nanoseconds;
};

/* The guest is i386 even though this syscall table uses arm64_32 numbers.
 * Linux i386 struct iovec is two 32-bit words; never reinterpret this as the
 * native arm64 `struct iovec`. */
struct guest_iovec32
{
	uint32_t base;
	uint32_t length;
};

_Static_assert(sizeof(struct guest_iovec32) == 8, "i386 iovec layout");

struct futex_waiter
{
	struct futex_waiter *next;
	uint32_t guest_address;
	pthread_cond_t condition;
	int woken;
};

#define FUTEX_BUCKET_COUNT 64
struct futex_bucket
{
	pthread_mutex_t mutex;
	struct futex_waiter *waiters;
};

static struct futex_bucket futex_buckets[FUTEX_BUCKET_COUNT];
static pthread_once_t futex_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t vm_ops_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct mac_guest_vm_ops vm_ops;
static int vm_ops_installed;
static _Atomic uint32_t next_guest_tid = 1;
static _Thread_local uint32_t current_guest_tid;

static void initialize_futex_buckets(void)
{
	for (size_t index = 0; index < FUTEX_BUCKET_COUNT; ++index)
		pthread_mutex_init(&futex_buckets[index].mutex, NULL);
}

int mac_guest_vm_ops_install(const struct mac_guest_vm_ops *operations)
{
	int result = -1;

	if (!operations || !operations->map || !operations->unmap || !operations->protect)
		return -1;
	pthread_mutex_lock(&vm_ops_mutex);
	if (!vm_ops_installed)
	{
		vm_ops = *operations;
		vm_ops_installed = 1;
		result = 0;
	}
	pthread_mutex_unlock(&vm_ops_mutex);
	return result;
}

static struct mac_guest_vm_ops get_vm_ops(int *installed)
{
	struct mac_guest_vm_ops result;
	pthread_mutex_lock(&vm_ops_mutex);
	*installed = vm_ops_installed;
	result = vm_ops;
	pthread_mutex_unlock(&vm_ops_mutex);
	return result;
}

static uint32_t guest_tid(void)
{
	if (!current_guest_tid)
	{
		current_guest_tid = atomic_fetch_add_explicit(&next_guest_tid, 1,
			memory_order_relaxed);
		if (!current_guest_tid)
			current_guest_tid = atomic_fetch_add_explicit(&next_guest_tid, 1,
				memory_order_relaxed);
	}
	return current_guest_tid;
}

static int64_t syscall_mmap(const uint64_t a[6])
{
	struct mac_guest_vm_ops operations;
	int installed;
	uint32_t mapped = 0;
	/* Although the syscall number table is arm64_32, this project's guest
	 * objects use ILP32 pointers and pass mmap's sixth argument as byte offset. */
	uint64_t offset = a[5];
	int64_t result;

	operations = get_vm_ops(&installed);
	if (!installed)
		return -_guest_enosys;
	result = operations.map(operations.context, (uint32_t)a[0], (uint32_t)a[1],
		(int)a[2], (uint32_t)a[3], (int32_t)a[4], offset, &mapped);
	if (result != 0)
		return result;
	if (!mapped || !mac_guest_address_resolve(mapped, (uint32_t)a[1]))
		return -_guest_efault;
	return mapped;
}

static int64_t syscall_munmap(uint32_t address, uint32_t size)
{
	struct mac_guest_vm_ops operations;
	int installed;
	operations = get_vm_ops(&installed);
	return installed ? operations.unmap(operations.context, address, size) : -_guest_enosys;
}

static int64_t syscall_mprotect(uint32_t address, uint32_t size, int protection)
{
	struct mac_guest_vm_ops operations;
	int installed;
	if (protection & ~(_guest_prot_read | _guest_prot_write | _guest_prot_exec))
		return -_guest_einval;
	operations = get_vm_ops(&installed);
	return installed ? operations.protect(operations.context, address, size, protection) : -_guest_enosys;
}

static int64_t syscall_nanosleep(uint32_t request_va, uint32_t remaining_va)
{
	struct guest_timespec32 request;
	struct timespec native_request, native_remaining;
	struct guest_timespec32 guest_remaining;
	if (mac_guest_read(request_va, &request, sizeof(request)) != 0)
		return -_guest_efault;
	if (request.seconds < 0 || request.nanoseconds < 0 || request.nanoseconds >= 1000000000)
		return -_guest_einval;
	native_request.tv_sec = request.seconds;
	native_request.tv_nsec = request.nanoseconds;
	if (nanosleep(&native_request, &native_remaining) == 0)
		return 0;
	if (errno == EINTR && remaining_va)
	{
		guest_remaining.seconds = (int32_t)native_remaining.tv_sec;
		guest_remaining.nanoseconds = (int32_t)native_remaining.tv_nsec;
		if (mac_guest_write(remaining_va, &guest_remaining, sizeof(guest_remaining)) != 0)
			return -_guest_efault;
	}
	return -mac_host_linux_errno(errno);
}

/* Darwin does not provide clock_nanosleep. Resolve an absolute wall/monotonic
 * deadline against that same clock, then use nanosleep for the remaining span.
 * Preserve EINTR and Linux's rule that absolute requests do not write rem. */
static int64_t syscall_clock_nanosleep(int clock, int flags, uint32_t request_va,
	uint32_t remaining_va, int time64)
{
	int64_t seconds, nanoseconds;
	clockid_t native_clock;
	if (clock == 0) native_clock = CLOCK_REALTIME;
	else if (clock == 1) native_clock = CLOCK_MONOTONIC;
	else return -_guest_einval;
	if (flags != 0 && flags != 1) return -_guest_einval;
	if (time64)
	{
		int64_t request[2];
		if (mac_guest_read(request_va, request, sizeof(request))) return -_guest_efault;
		seconds = request[0]; nanoseconds = request[1];
	}
	else
	{
		struct guest_timespec32 request;
		if (mac_guest_read(request_va, &request, sizeof(request))) return -_guest_efault;
		seconds = request.seconds; nanoseconds = request.nanoseconds;
	}
	if (seconds < 0 || nanoseconds < 0 || nanoseconds >= 1000000000) return -_guest_einval;
	if (!flags && remaining_va && !mac_guest_address_resolve(remaining_va, time64 ? 16 : 8))
		return -_guest_efault;
	struct timespec span = {(time_t)seconds, (long)nanoseconds}, remaining;
	if (flags)
	{
		struct timespec now;
		if (clock_gettime(native_clock, &now)) return -mac_host_linux_errno(errno);
		if (seconds < now.tv_sec || (seconds == now.tv_sec && nanoseconds <= now.tv_nsec)) return 0;
		span.tv_sec -= now.tv_sec;
		span.tv_nsec -= now.tv_nsec;
		if (span.tv_nsec < 0) { --span.tv_sec; span.tv_nsec += 1000000000; }
	}
	if (nanosleep(&span, &remaining) == 0) return 0;
	int error = errno;
	if (error == EINTR && !flags && remaining_va)
	{
		if (time64)
		{
			int64_t value[2] = {remaining.tv_sec, remaining.tv_nsec};
			if (mac_guest_write(remaining_va, value, sizeof(value))) return -_guest_efault;
		}
		else
		{
			struct guest_timespec32 value = {(int32_t)remaining.tv_sec, (int32_t)remaining.tv_nsec};
			if (mac_guest_write(remaining_va, &value, sizeof(value))) return -_guest_efault;
		}
	}
	return -mac_host_linux_errno(error);
}

static int64_t syscall_vector_io(int fd, uint32_t guest_iov_va, int32_t count, int writing)
{
	struct guest_iovec32 guest_iov[1024];
	struct iovec host_iov[1024];
	uint64_t total = 0;
	ssize_t written;

	if (count < 0 || count > (int32_t)(sizeof(guest_iov) / sizeof(guest_iov[0])))
		return -mac_host_linux_errno(EINVAL);
	if (count == 0)
	{
		int flags = fcntl(fd, F_GETFL);
		if (flags < 0)
			return -mac_host_linux_errno(errno);
		if ((flags & O_ACCMODE) == (writing ? O_RDONLY : O_WRONLY))
			return -_guest_ebadf;
		return 0;
	}
	if (mac_guest_read(guest_iov_va, guest_iov,
		(size_t)count * sizeof(guest_iov[0])) != 0)
		return -_guest_efault;
	for (int32_t index = 0; index < count; ++index)
	{
		/* ssize_t is signed 32-bit in the guest, even on the 64-bit host. */
		if (guest_iov[index].length > (uint64_t)INT32_MAX - total)
			return -mac_host_linux_errno(EINVAL);
		total += guest_iov[index].length;
		host_iov[index].iov_len = guest_iov[index].length;
		host_iov[index].iov_base = guest_iov[index].length ?
			mac_guest_address_resolve(guest_iov[index].base, guest_iov[index].length) : NULL;
		if (guest_iov[index].length && !host_iov[index].iov_base)
			return -_guest_efault;
	}
	written = writing ? writev(fd, host_iov, count) : readv(fd, host_iov, count);
	return written < 0 ? -mac_host_linux_errno(errno) : (int64_t)written;
}

static struct futex_bucket *futex_bucket_for(uint32_t address)
{
	return &futex_buckets[(address >> 2) % FUTEX_BUCKET_COUNT];
}

static int64_t syscall_futex_wait(uint32_t address, uint32_t expected, uint32_t timeout_va)
{
	struct futex_bucket *bucket;
	struct futex_waiter waiter;
	struct guest_timespec32 timeout;
	struct timespec absolute_timeout;
	uint32_t *word;
	int has_timeout = timeout_va != 0;
	int wait_error = 0;

	if ((address & 3u) || !(word = mac_guest_address_resolve(address, sizeof(*word))))
		return -_guest_efault;
	if (has_timeout && mac_guest_read(timeout_va, &timeout, sizeof(timeout)) != 0)
		return -_guest_efault;
	if (has_timeout && (timeout.seconds < 0 || timeout.nanoseconds < 0 ||
		timeout.nanoseconds >= 1000000000))
		return -_guest_einval;
	if (has_timeout)
	{
		if (clock_gettime(CLOCK_REALTIME, &absolute_timeout) != 0)
			return -mac_host_linux_errno(errno);
		absolute_timeout.tv_sec += timeout.seconds;
		absolute_timeout.tv_nsec += timeout.nanoseconds;
		if (absolute_timeout.tv_nsec >= 1000000000)
		{
			absolute_timeout.tv_sec++;
			absolute_timeout.tv_nsec -= 1000000000;
		}
	}
	pthread_once(&futex_once, initialize_futex_buckets);
	bucket = futex_bucket_for(address);
	waiter = (struct futex_waiter){ 0 };
	waiter.guest_address = address;
	pthread_cond_init(&waiter.condition, NULL);
	pthread_mutex_lock(&bucket->mutex);
	if (__atomic_load_n(word, __ATOMIC_ACQUIRE) != expected)
	{
		pthread_mutex_unlock(&bucket->mutex);
		pthread_cond_destroy(&waiter.condition);
		return -_guest_eagain;
	}
	waiter.next = bucket->waiters;
	bucket->waiters = &waiter;
	while (!waiter.woken)
	{
		wait_error = has_timeout ? pthread_cond_timedwait(&waiter.condition,
			&bucket->mutex, &absolute_timeout) : pthread_cond_wait(&waiter.condition,
			&bucket->mutex);
		if (wait_error == ETIMEDOUT)
			break;
		if (wait_error != 0)
			break;
	}
	{
		struct futex_waiter **link = &bucket->waiters;
		while (*link && *link != &waiter)
			link = &(*link)->next;
		if (*link)
			*link = waiter.next;
	}
	pthread_mutex_unlock(&bucket->mutex);
	pthread_cond_destroy(&waiter.condition);
	if (wait_error == ETIMEDOUT)
		return -_guest_etimedout;
	if (wait_error != 0)
		return -mac_host_linux_errno(wait_error);
	return 0;
}

static int64_t syscall_futex_wake(uint32_t address, int32_t count)
{
	struct futex_bucket *bucket;
	struct futex_waiter *waiter;
	int32_t woken = 0;
	if ((address & 3u) || !mac_guest_address_resolve(address, sizeof(uint32_t)))
		return -_guest_efault;
	if (count < 0)
		return -_guest_einval;
	pthread_once(&futex_once, initialize_futex_buckets);
	bucket = futex_bucket_for(address);
	pthread_mutex_lock(&bucket->mutex);
	for (waiter = bucket->waiters; waiter && woken < count; waiter = waiter->next)
	{
		if (waiter->guest_address == address && !waiter->woken)
		{
			waiter->woken = 1;
			pthread_cond_signal(&waiter->condition);
			woken++;
		}
	}
	pthread_mutex_unlock(&bucket->mutex);
	return woken;
}

static int64_t syscall_futex(uint32_t address, int operation, uint32_t value,
	uint32_t timeout_va)
{
	int command = operation & ~(int)(_guest_futex_private | _guest_futex_realtime);
	if ((operation & ~(_guest_futex_private | _guest_futex_realtime | 0x7f)) != 0)
		return -_guest_einval;
	switch (command)
	{
	case _guest_futex_wait:
		if (operation & _guest_futex_realtime)
			return -_guest_einval;
		return syscall_futex_wait(address, value, timeout_va);
	case _guest_futex_wake:
		return syscall_futex_wake(address, (int32_t)value);
	default:
		return -_guest_enosys;
	}
}

int64_t mac_host_arm64_32_syscall(uint64_t number, const uint64_t a[6])
{
	uint32_t guest_address, guest_size;
	if (!a || number > UINT32_MAX)
		return -_guest_enosys;
	switch ((uint32_t)number)
	{
	case _guest_exit:
		pthread_exit(NULL);
	case _guest_exit_group:
		mac_host_exit((int32_t)a[0]);
	case _guest_read:
		return mac_host_read((int32_t)a[0], (uint32_t)a[1], (uint32_t)a[2]);
	case _guest_write:
		return mac_host_write((int32_t)a[0], (uint32_t)a[1], (uint32_t)a[2]);
	case _guest_writev:
		return syscall_vector_io((int32_t)a[0], (uint32_t)a[1], (int32_t)a[2], 1);
	case _guest_readv:
		return syscall_vector_io((int32_t)a[0], (uint32_t)a[1], (int32_t)a[2], 0);
	case _guest_pread64:
		return mac_host_pread((int32_t)a[0], (uint32_t)a[1], (uint32_t)a[2],
			(int64_t)a[3]);
	case _guest_pwrite64:
		return mac_host_pwrite((int32_t)a[0], (uint32_t)a[1], (uint32_t)a[2],
			(int64_t)a[3]);
	case _guest_openat:
		return mac_host_openat((int32_t)a[0], (uint32_t)a[1], (uint32_t)a[2], (uint32_t)a[3]);
	case _guest_close:
		return mac_host_close((int32_t)a[0]);
	case _guest_lseek:
		return (int32_t)mac_host_lseek((int32_t)a[0], (int32_t)a[1], (int32_t)a[2]);
	case _guest_getpid:
		return (int64_t)getpid();
	case _guest_getuid:
		return (int64_t)getuid();
	case _guest_getgid:
		return (int64_t)getgid();
	case _guest_geteuid:
		return (int64_t)geteuid();
	case _guest_getegid:
		return (int64_t)getegid();
	case _guest_gettimeofday:
		return mac_host_gettimeofday((uint32_t)a[0]);
	case _guest_munmap:
		return syscall_munmap((uint32_t)a[0], (uint32_t)a[1]);
	case _guest_mprotect:
		return syscall_mprotect((uint32_t)a[0], (uint32_t)a[1], (int)a[2]);
	case _guest_sched_yield:
		return sched_yield() == 0 ? 0 : -mac_host_linux_errno(errno);
	case _guest_clock_nanosleep:
		return syscall_clock_nanosleep((int32_t)a[0], (int32_t)a[1], (uint32_t)a[2], (uint32_t)a[3], 0);
	case _guest_clock_nanosleep_time64:
		return syscall_clock_nanosleep((int32_t)a[0], (int32_t)a[1], (uint32_t)a[2], (uint32_t)a[3], 1);
	case _guest_nanosleep:
		return syscall_nanosleep((uint32_t)a[0], (uint32_t)a[1]);
	case _guest_gettid:
		return guest_tid();
	case _guest_futex:
		return syscall_futex((uint32_t)a[0], (int)a[1], (uint32_t)a[2], (uint32_t)a[3]);
	case _guest_clock_gettime:
		return mac_host_clock_gettime((int)a[0], (uint32_t)a[1]);
	case _guest_clock_gettime64:
		return mac_host_clock_gettime64((int)a[0], (uint32_t)a[1]);
	case _guest_mmap:
		return syscall_mmap(a);
	case _guest_getrandom:
		guest_address = (uint32_t)a[0];
		guest_size = (uint32_t)a[1];
		if (guest_size && !mac_guest_address_resolve(guest_address, guest_size))
			return -_guest_efault;
		if ((uint32_t)a[2] & ~3u)
			return -_guest_einval;
		if (guest_size)
			arc4random_buf(mac_guest_address_resolve(guest_address, guest_size), guest_size);
		return guest_size;
	default:
		return -_guest_enosys;
	}
}

int64_t mac_host_arm64_32_syscall_from_stack(uint32_t guest_esp)
{
	uint64_t values[7];
	for (uint32_t index = 0; index < 7; ++index)
	{
		if (mac_guest_stack_arg64(guest_esp, index, &values[index]) != 0)
			return -_guest_efault;
	}
	return mac_host_arm64_32_syscall(values[0], &values[1]);
}
