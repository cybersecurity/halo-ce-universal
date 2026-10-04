/* Translate the ARM guest's Linux syscall ABI into Darwin calls. */
#include "host.h"
#include "port/android/guest/libc/arch/arm64_32/bits/syscall.h.in"
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#define G(t, p) ((t)guest_pointer(p))
struct guest_time {
    int32_t seconds, nanoseconds;
};
struct guest_vec {
    uint32_t address, length;
};
struct guest_stat {
    uint64_t dev, ino;
    uint32_t mode, nlink, uid, gid;
    uint64_t rdev;
    int64_t size;
    int32_t blksize;
    int32_t pad;
    int64_t blocks;
    struct guest_time atime, mtime, ctime;
};
struct guest_sysinfo {
    uint32_t uptime, loads[3], totalram, freeram, sharedram, bufferram, totalswap, freeswap;
    uint16_t procs, pad;
    uint32_t totalhigh, freehigh, mem_unit;
    char reserved[256];
};
static long result(long r) { return r == -1 ? -host_linux_errno(errno) : r; }
static struct timespec time_in(uint64_t p) {
    struct guest_time *g = G(struct guest_time *, p);
    return (struct timespec){g->seconds, g->nanoseconds};
}
static void time_out(uint64_t p, struct timespec t) {
    if (p)
        *G(struct guest_time *, p) = (struct guest_time){(int32_t)t.tv_sec, (int32_t)t.tv_nsec};
}
static clockid_t clock_id(int c) {
    return c == 0   ? CLOCK_REALTIME
           : c == 2 ? CLOCK_PROCESS_CPUTIME_ID
           : c == 3 ? CLOCK_THREAD_CPUTIME_ID
                    : CLOCK_MONOTONIC;
}
static int dirfd_in(int f) { return f == -100 ? AT_FDCWD : f; }
static int open_flags(int f) {
    int out = f & 3;
    if (f & 64)
        out |= O_CREAT;
    if (f & 128)
        out |= O_EXCL;
    if (f & 512)
        out |= O_TRUNC;
    if (f & 1024)
        out |= O_APPEND;
    if (f & 2048)
        out |= O_NONBLOCK;
    if (f & 0x80000)
        out |= O_CLOEXEC;
    if (f & 0x10000)
        out |= O_DIRECTORY;
    return out;
}
static long stat_out(int fd, const char *path, uint64_t p, int flags) {
    struct stat s;
    int r = path ? fstatat(dirfd_in(fd), path, &s, (flags & 0x100) ? AT_SYMLINK_NOFOLLOW : 0)
                 : fstat(fd, &s);
    if (r)
        return result(r);
    struct guest_stat *g = G(struct guest_stat *, p);
    memset(g, 0, sizeof(*g));
    g->dev = s.st_dev;
    g->ino = s.st_ino;
    g->mode = s.st_mode;
    g->nlink = s.st_nlink;
    g->uid = s.st_uid;
    g->gid = s.st_gid;
    g->rdev = s.st_rdev;
    g->size = s.st_size;
    g->blksize = s.st_blksize;
    g->blocks = s.st_blocks;
    g->atime = (struct guest_time){(int32_t)s.st_atimespec.tv_sec, (int32_t)s.st_atimespec.tv_nsec};
    g->mtime = (struct guest_time){(int32_t)s.st_mtimespec.tv_sec, (int32_t)s.st_mtimespec.tv_nsec};
    g->ctime = (struct guest_time){(int32_t)s.st_ctimespec.tv_sec, (int32_t)s.st_ctimespec.tv_nsec};
    return 0;
}
struct waiter {
    uint32_t address;
    int woken;
    pthread_cond_t condition;
    struct waiter *next;
};
static pthread_mutex_t futex_lock = PTHREAD_MUTEX_INITIALIZER;
static struct waiter *waiters;
static long futex(uint32_t address, int op, uint32_t value, uint64_t timeout) {
    int command = op & 127;
    pthread_mutex_lock(&futex_lock);
    if (command == 0 || command == 9) {
        if (__atomic_load_n(G(uint32_t *, address), __ATOMIC_SEQ_CST) != value) {
            pthread_mutex_unlock(&futex_lock);
            return -11;
        }
        struct waiter w = {
            .address = address, .condition = PTHREAD_COND_INITIALIZER, .next = waiters};
        waiters = &w;
        int error = 0;
        struct timespec deadline;
        if (timeout) {
            deadline = time_in(timeout);
            if (command == 0) {
                struct timespec now;
                clock_gettime(CLOCK_REALTIME, &now);
                deadline.tv_sec += now.tv_sec;
                deadline.tv_nsec += now.tv_nsec;
            } else if (!(op & 256)) {
                struct timespec real, mono;
                clock_gettime(CLOCK_REALTIME, &real);
                clock_gettime(CLOCK_MONOTONIC, &mono);
                deadline.tv_sec += real.tv_sec - mono.tv_sec;
                deadline.tv_nsec += real.tv_nsec - mono.tv_nsec;
            }
            if (deadline.tv_nsec >= 1000000000) {
                deadline.tv_sec++;
                deadline.tv_nsec -= 1000000000;
            }
            if (deadline.tv_nsec < 0) {
                deadline.tv_sec--;
                deadline.tv_nsec += 1000000000;
            }
        }
        while (!w.woken && !error)
            error = timeout ? pthread_cond_timedwait(&w.condition, &futex_lock, &deadline)
                            : pthread_cond_wait(&w.condition, &futex_lock);
        struct waiter **cursor = &waiters;
        while (*cursor != &w)
            cursor = &(*cursor)->next;
        *cursor = w.next;
        pthread_mutex_unlock(&futex_lock);
        pthread_cond_destroy(&w.condition);
        return error ? -host_linux_errno(error) : 0;
    }
    if (command == 1 || command == 10) {
        long count = 0;
        for (struct waiter *w = waiters; w && count < value; w = w->next)
            if (w->address == address && !w->woken) {
                w->woken = 1;
                pthread_cond_signal(&w->condition);
                count++;
            }
        pthread_mutex_unlock(&futex_lock);
        return count;
    }
    pthread_mutex_unlock(&futex_lock);
    return -38;
}
long long host_syscall(long long number, long long a, long long b, long long c, long long d,
                       long long e, long long f) {
    switch (number) {
    case __NR_read:
        return result(read((int)a, G(void *, b), (uint32_t)c));
    case __NR_write:
        return result(write((int)a, G(const void *, b), (uint32_t)c));
    case __NR_pread64:
        return result(pread((int)a, G(void *, b), (uint32_t)c, d));
    case __NR_pwrite64:
        return result(pwrite((int)a, G(const void *, b), (uint32_t)c, d));
    case __NR_readv:
    case __NR_writev:
    case __NR_preadv:
    case __NR_pwritev: {
        if (c < 0 || c > 64)
            return -22;
        struct iovec v[64];
        struct guest_vec *g = G(struct guest_vec *, b);
        for (int i = 0; i < c; i++)
            v[i] = (struct iovec){guest_pointer(g[i].address), g[i].length};
        if (number == __NR_readv)
            return result(readv((int)a, v, (int)c));
        if (number == __NR_writev)
            return result(writev((int)a, v, (int)c));
        if (number == __NR_preadv)
            return result(preadv((int)a, v, (int)c, d));
        return result(pwritev((int)a, v, (int)c, d));
    }
    case __NR_openat:
        return result(openat(dirfd_in((int)a), G(const char *, b), open_flags((int)c), (mode_t)d));
    case __NR_close:
        return result(close((int)a));
    case __NR_lseek:
        return result(lseek((int)a, b, (int)c));
    case __NR_fstat:
        return stat_out((int)a, NULL, b, 0);
    case __NR_newfstatat:
        return stat_out((int)a, G(const char *, b), c, (int)d);
    case __NR_ftruncate:
        return result(ftruncate((int)a, b));
    case __NR_fsync:
    case __NR_fdatasync:
        return result(fsync((int)a));
    case __NR_fcntl:
        if (b == 1)
            return result(fcntl((int)a, F_GETFD));
        if (b == 2)
            return result(fcntl((int)a, F_SETFD, (int)c));
        if (b == 3)
            return result(fcntl((int)a, F_GETFL));
        if (b == 4)
            return result(fcntl((int)a, F_SETFL, open_flags((int)c)));
        return -22;
    case __NR_unlinkat:
        return result(
            unlinkat(dirfd_in((int)a), G(const char *, b), (c & 0x200) ? AT_REMOVEDIR : 0));
    case __NR_renameat:
        return result(
            renameat(dirfd_in((int)a), G(const char *, b), dirfd_in((int)c), G(const char *, d)));
    case __NR_mkdirat:
        return result(mkdirat(dirfd_in((int)a), G(const char *, b), (mode_t)c));
    case __NR_fchmod:
        return result(fchmod((int)a, (mode_t)b));
    case __NR_faccessat:
        return result(faccessat(dirfd_in((int)a), G(const char *, b), (int)c, 0));
    case __NR_chdir:
        return result(chdir(G(const char *, a)));
    case __NR_getcwd:
        return getcwd(G(char *, a), (uint32_t)b) ? (long long)strlen(G(char *, a)) + 1
                                                 : -host_linux_errno(errno);
    case __NR_readlinkat:
        return result(readlinkat(dirfd_in((int)a), G(const char *, b), G(char *, c), (uint32_t)d));
    case __NR_clock_gettime:
    case __NR_clock_getres: {
        struct timespec t;
        int r = number == __NR_clock_gettime ? clock_gettime(clock_id((int)a), &t)
                                             : clock_getres(clock_id((int)a), &t);
        if (!r)
            time_out(b, t);
        return result(r);
    }
    case __NR_gettimeofday: {
        struct timeval t;
        gettimeofday(&t, NULL);
        if (a)
            *G(struct guest_time *, a) = (struct guest_time){(int32_t)t.tv_sec, t.tv_usec};
        return 0;
    }
    case __NR_nanosleep: {
        struct timespec t = time_in(a), remaining = {0};
        int r = nanosleep(&t, &remaining);
        if (r)
            time_out(b, remaining);
        return result(r);
    }
    case __NR_clock_nanosleep: {
        struct timespec t = time_in(c), remaining = {0};
        if (b) {
            struct timespec now;
            clock_gettime(clock_id((int)a), &now);
            t.tv_sec -= now.tv_sec;
            t.tv_nsec -= now.tv_nsec;
            if (t.tv_nsec < 0) {
                t.tv_sec--;
                t.tv_nsec += 1000000000;
            }
            if (t.tv_sec < 0)
                return 0;
        }
        int r = nanosleep(&t, &remaining);
        if (r)
            time_out(d, remaining);
        return result(r);
    }
    case __NR_futex:
        return futex((uint32_t)a, (int)b, (uint32_t)c, d);
    case __NR_mmap:
        return host_guest_mmap((uint32_t)a, (uint32_t)b, (int)c, (int)d, (int)e, f);
    case __NR_munmap:
        return host_guest_munmap((uint32_t)a, (uint32_t)b);
    case __NR_mprotect:
        return host_guest_mprotect((uint32_t)a, (uint32_t)b, (int)c);
    case __NR_madvise:
        return 0;
    case __NR_mremap:
    case __NR_brk:
        return -12;
    case __NR_ioctl:
        return -25;
    case __NR_sched_yield:
        sched_yield();
        return 0;
    case __NR_gettid:
    case __NR_set_tid_address: {
        uint64_t tid;
        pthread_threadid_np(NULL, &tid);
        return (uint32_t)tid;
    }
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
    case __NR_getrandom:
        arc4random_buf(G(void *, a), (uint32_t)b);
        return (uint32_t)b;
    case __NR_sysinfo: {
        struct guest_sysinfo *info = G(struct guest_sysinfo *, a);
        memset(info, 0, sizeof(*info));
        uint64_t bytes = 0;
        size_t size = sizeof(bytes);
        if (sysctlbyname("hw.memsize", &bytes, &size, NULL, 0))
            return -host_linux_errno(errno);
        info->mem_unit = HALO_MACOS_PAGE;
        info->totalram = (uint32_t)(bytes / HALO_MACOS_PAGE);
        vm_statistics64_data_t vm;
        mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
        if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vm, &count) ==
            KERN_SUCCESS)
            info->freeram = vm.free_count + vm.inactive_count;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        info->uptime = (uint32_t)now.tv_sec;
        return 0;
    }
    case __NR_rt_sigaction:
    case __NR_rt_sigprocmask:
    case __NR_sigaltstack:
    case __NR_membarrier:
        return 0;
    case __NR_ppoll: {
        int ms = -1;
        if (c) {
            struct timespec t = time_in(c);
            ms = (int)(t.tv_sec * 1000 + (t.tv_nsec + 999999) / 1000000);
        }
        return result(poll(G(struct pollfd *, a), (uint32_t)b, ms));
    }
    case __NR_flock:
        return result(flock((int)a, (int)b));
    case __NR_umask:
        return umask((mode_t)a);
    case __NR_exit:
    case __NR_exit_group:
        host_exit((int)a);
    default:
        host_logf(HOST_LOG_WARN, "unimplemented guest syscall %lld", number);
        return -38;
    }
}
