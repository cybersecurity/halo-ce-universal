/* Translate the guest's Linux/musl ABI into Darwin libc and pthread calls. */
#include "ios_host.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <time.h>
#include <unistd.h>
#include "guest_syscall_numbers.h"

#define P(type,x) ((type)host_pointer((uint64_t)(x)))
struct guest_timespec { int32_t seconds, nanoseconds; };
struct guest_iovec { uint32_t pointer, length; };
struct guest_stat {
    uint64_t device,inode;
    uint32_t mode,links,uid,gid;
    uint64_t rdev;
    int64_t size;
    int32_t block_size,padding;
    int64_t blocks;
    struct guest_timespec atime,mtime,ctime;
};
_Static_assert(sizeof(struct guest_stat)==88,"guest stat layout");

/* Linux ILP32 sysinfo. Counts are pages to avoid truncating a device's RAM. */
struct guest_sysinfo {
    uint32_t uptime, loads[3], totalram, freeram, sharedram, bufferram;
    uint32_t totalswap, freeswap;
    uint16_t procs, pad;
    uint32_t totalhigh, freehigh, mem_unit;
    char reserved[256];
};
static long guest_sysinfo(uint64_t address) {
    struct guest_sysinfo *info=P(struct guest_sysinfo *,address);
    uint64_t memory=0; size_t size=sizeof(memory);
    if(sysctlbyname("hw.memsize",&memory,&size,NULL,0))return -5;
    memset(info,0,sizeof(*info));
    struct timespec uptime;clock_gettime(CLOCK_MONOTONIC,&uptime);
    info->uptime=(uint32_t)uptime.tv_sec;
    info->mem_unit=(uint32_t)getpagesize();
    info->totalram=(uint32_t)(memory/info->mem_unit);
    vm_statistics64_data_t stats;mach_msg_type_number_t count=HOST_VM_INFO64_COUNT;
    if(host_statistics64(mach_host_self(),HOST_VM_INFO64,(host_info64_t)&stats,&count)==KERN_SUCCESS)
        info->freeram=stats.free_count;
    return 0;
}

int host_linux_errno(int e) {
    switch(e) {
        case EAGAIN:return 11; case EDEADLK:return 35; case ENAMETOOLONG:return 36;
        case ENOLCK:return 37; case ENOSYS:return 38; case ENOTEMPTY:return 39;
        case ELOOP:return 40; case EOVERFLOW:return 75; case EILSEQ:return 84;
        case ENOTSOCK:return 88; case EMSGSIZE:return 90; case ENOTSUP:return 95;
        case EADDRINUSE:return 98; case ECONNRESET:return 104;
        case ETIMEDOUT:return 110; case ECONNREFUSED:return 111;
        case EINPROGRESS:return 115; case ECANCELED:return 125;
        default:return e;
    }
}
static long long result(long long n) { return n==-1 ? -host_linux_errno(errno) : n; }
static int directory_fd(int n) { return n==-100 ? AT_FDCWD : n; }
static int open_flags(int n) {
    int out=n&3;
    if(n&0100)out|=O_CREAT;if(n&0200)out|=O_EXCL;if(n&01000)out|=O_TRUNC;
    if(n&02000)out|=O_APPEND;if(n&04000)out|=O_NONBLOCK;if(n&02000000)out|=O_CLOEXEC;
    if(n&040000)out|=O_DIRECTORY;if(n&0100000)out|=O_NOFOLLOW;
    return out;
}
static clockid_t clock_id(int n) { return n==0?CLOCK_REALTIME:CLOCK_MONOTONIC; }
static struct timespec time_in(uint64_t p) {
    const struct guest_timespec *t=P(const struct guest_timespec *,p);
    return t?(struct timespec){t->seconds,t->nanoseconds}:(struct timespec){0,0};
}
static void time_out(uint64_t p,struct timespec t) {
    struct guest_timespec *g=P(struct guest_timespec *,p);
    if(g) {g->seconds=(int32_t)t.tv_sec;g->nanoseconds=(int32_t)t.tv_nsec;}
}
static void stat_out(uint64_t p,const struct stat *s) {
    struct guest_stat *g=P(struct guest_stat *,p);
    memset(g,0,sizeof(*g));g->device=s->st_dev;g->inode=s->st_ino;g->mode=s->st_mode;
    g->links=s->st_nlink;g->uid=s->st_uid;g->gid=s->st_gid;g->rdev=s->st_rdev;
    g->size=s->st_size;g->block_size=s->st_blksize;g->blocks=s->st_blocks;
    g->atime=(struct guest_timespec){s->st_atimespec.tv_sec,s->st_atimespec.tv_nsec};
    g->mtime=(struct guest_timespec){s->st_mtimespec.tv_sec,s->st_mtimespec.tv_nsec};
    g->ctime=(struct guest_timespec){s->st_ctimespec.tv_sec,s->st_ctimespec.tv_nsec};
}

/* Each futex waiter has its own condition: hash collisions cannot consume
   a wakeup intended for another guest address. */
struct waiter { uint32_t address; int signaled; pthread_cond_t condition; struct waiter *next; };
static struct waiter *waiters;
static pthread_mutex_t futex_lock=PTHREAD_MUTEX_INITIALIZER;
static long futex(uint32_t address,int operation,uint32_t value,uint64_t timeout) {
    int command=operation&127;
    pthread_mutex_lock(&futex_lock);
    if(command==1 || command==10) {
        unsigned woke=0;
        for(struct waiter *w=waiters;w && woke<value;w=w->next)
            if(w->address==address && !w->signaled) { w->signaled=1;pthread_cond_signal(&w->condition);woke++; }
        pthread_mutex_unlock(&futex_lock);return woke;
    }
    if(command!=0 && command!=9) {pthread_mutex_unlock(&futex_lock);return -38;}
    if(__atomic_load_n(P(uint32_t *,address),__ATOMIC_SEQ_CST)!=value) {pthread_mutex_unlock(&futex_lock);return -11;}
    struct waiter w={.address=address,.next=waiters};pthread_cond_init(&w.condition,NULL);waiters=&w;
    int error=0;struct timespec deadline;
    if(timeout) {
        struct timespec delta=time_in(timeout);clock_gettime(CLOCK_REALTIME,&deadline);
        if(command==9) {
            struct timespec now;clock_gettime((operation&256)?CLOCK_REALTIME:CLOCK_MONOTONIC,&now);
            delta.tv_sec-=now.tv_sec;delta.tv_nsec-=now.tv_nsec;
        }
        deadline.tv_sec+=delta.tv_sec;deadline.tv_nsec+=delta.tv_nsec;
        while(deadline.tv_nsec>=1000000000){deadline.tv_sec++;deadline.tv_nsec-=1000000000;}
        while(deadline.tv_nsec<0){deadline.tv_sec--;deadline.tv_nsec+=1000000000;}
    }
    while(!w.signaled && !error) error=timeout?pthread_cond_timedwait(&w.condition,&futex_lock,&deadline):pthread_cond_wait(&w.condition,&futex_lock);
    struct waiter **link=&waiters;while(*link!=&w)link=&(*link)->next;*link=w.next;
    pthread_cond_destroy(&w.condition);pthread_mutex_unlock(&futex_lock);
    return w.signaled?0:-host_linux_errno(error);
}

long long host_syscall(long long number,long long a,long long b,long long c,long long d,long long e,long long f) {
    switch(number) {
    case __NR_read:return result(read((int)a,P(void *,b),(uint32_t)c));
    case __NR_write:return result(write((int)a,P(const void *,b),(uint32_t)c));
    case __NR_openat:return result(openat(directory_fd(a),P(const char *,b),open_flags(c),(mode_t)d));
    case __NR_close:return result(close(a));
    case __NR_lseek:return result(lseek(a,b,c));
    case __NR_pread64:return result(pread(a,P(void *,b),(uint32_t)c,d));
    case __NR_pwrite64:return result(pwrite(a,P(const void *,b),(uint32_t)c,d));
    case __NR_readv:case __NR_writev:case __NR_preadv:case __NR_pwritev: {
        if(c<0 || c>64)return -22;
        struct iovec vector[64];struct guest_iovec *g=P(struct guest_iovec *,b);
        for(int i=0;i<c;i++)vector[i]=(struct iovec){host_pointer(g[i].pointer),g[i].length};
        if(number==__NR_readv)return result(readv(a,vector,c));
        if(number==__NR_writev)return result(writev(a,vector,c));
        if(number==__NR_preadv)return result(preadv(a,vector,c,d));
        return result(pwritev(a,vector,c,d));
    }
    case __NR_fstat:case __NR_newfstatat: {
        struct stat st;int rc=number==__NR_fstat?fstat(a,&st):fstatat(directory_fd(a),P(const char *,b),&st,(d&0x100)?AT_SYMLINK_NOFOLLOW:0);
        if(!rc)stat_out(number==__NR_fstat?b:c,&st);return result(rc);
    }
    case __NR_ftruncate:return result(ftruncate(a,b));
    case __NR_fsync:case __NR_fdatasync:return result(fsync(a));
    case __NR_mkdirat:return result(mkdirat(directory_fd(a),P(const char *,b),c));
    case __NR_unlinkat:return result(unlinkat(directory_fd(a),P(const char *,b),(c&0x200)?AT_REMOVEDIR:0));
    case __NR_renameat:return result(renameat(directory_fd(a),P(const char *,b),directory_fd(c),P(const char *,d)));
    case __NR_faccessat:return result(faccessat(directory_fd(a),P(const char *,b),c,0));
    case __NR_readlinkat:return result(readlinkat(directory_fd(a),P(const char *,b),P(char *,c),d));
    case __NR_getcwd:return getcwd(P(char *,a),b)?strlen(P(char *,a))+1:-host_linux_errno(errno);
    case __NR_chdir:return result(chdir(P(const char *,a)));
    case __NR_fchmod:return result(fchmod(a,b));
    case __NR_umask:return umask(a);
    case __NR_fcntl:
        if(b==0 || b==1 || b==2)return result(fcntl(a,b,(int)c));
        if(b==3){int flags=fcntl(a,F_GETFL);if(flags<0)return result(flags);return (flags&3)|((flags&O_APPEND)?02000:0)|((flags&O_NONBLOCK)?04000:0);}
        if(b==4)return result(fcntl(a,F_SETFL,open_flags(c)));
        return -38;
    case __NR_dup:return result(dup(a));
    case __NR_ioctl:return -25;
    case __NR_clock_gettime:case __NR_clock_getres: {
        struct timespec t;int rc=number==__NR_clock_gettime?clock_gettime(clock_id(a),&t):clock_getres(clock_id(a),&t);
        if(!rc)time_out(b,t);return result(rc);
    }
    case __NR_gettimeofday:{struct timeval t;gettimeofday(&t,NULL);time_out(a,(struct timespec){t.tv_sec,t.tv_usec});return 0;}
    case __NR_nanosleep:case __NR_clock_nanosleep: {
        uint64_t request=number==__NR_nanosleep?a:c,remaining=number==__NR_nanosleep?b:d;
        struct timespec t=time_in(request),left={0};
        if(number==__NR_clock_nanosleep && b==1){struct timespec now;clock_gettime(clock_id(a),&now);t.tv_sec-=now.tv_sec;t.tv_nsec-=now.tv_nsec;if(t.tv_nsec<0){t.tv_sec--;t.tv_nsec+=1000000000;}if(t.tv_sec<0)return 0;}
        int rc=nanosleep(&t,&left);if(rc)time_out(remaining,left);return result(rc);
    }
    case __NR_futex:return futex(a,b,c,d);
    case __NR_sched_yield:return result(sched_yield());
    case __NR_sysinfo:return guest_sysinfo(a);
    case __NR_ppoll:{struct timespec t=time_in(c);return result(poll(P(struct pollfd *,a),b,c?(int)(t.tv_sec*1000+t.tv_nsec/1000000):-1));}
    case __NR_mmap:return host_guest_mmap(a,b,c,d,e,f);
    case __NR_munmap:return host_guest_munmap(a,b);
    case __NR_mprotect:return host_guest_mprotect(a,b,c);
    case __NR_madvise:return 0;
    case __NR_brk:case __NR_mremap:return -12;
    case __NR_getpid:return getpid();case __NR_getppid:return getppid();
    case __NR_getuid:case __NR_geteuid:return getuid();case __NR_getgid:case __NR_getegid:return getgid();
    case __NR_gettid:case __NR_set_tid_address:{uint64_t tid;pthread_threadid_np(NULL,&tid);return (uint32_t)tid;}
    case __NR_rt_sigaction:case __NR_rt_sigprocmask:case __NR_sigaltstack:case __NR_membarrier:return 0;
    case __NR_getrandom:arc4random_buf(P(void *,a),b);return b;
    case __NR_exit:case __NR_exit_group:host_exit(a);
    default:host_logf(HOST_LOG_WARN,"unsupported guest syscall %lld",number);return -38;
    }
}
