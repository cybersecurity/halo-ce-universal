/* The guest cannot enter the kernel directly with its own structure
layouts, so every system call musl makes goes to the runtime's dispatcher
(guest/runtime/guest_syscall.c), which performs it through the host. */
#define __SYSCALL_LL_E(x) (x)
#define __SYSCALL_LL_O(x) (x)

#define __scc(X) sizeof(1?(X):0ULL) < 8 ? (unsigned long) (X) : (long long) (X)
typedef long long syscall_arg_t;

long __guest_syscall(long long, long long, long long, long long, long long, long long, long long);

static __inline long __syscall0(long long n)
{
	return __guest_syscall(n, 0, 0, 0, 0, 0, 0);
}

static __inline long __syscall1(long long n, long long a)
{
	return __guest_syscall(n, a, 0, 0, 0, 0, 0);
}

static __inline long __syscall2(long long n, long long a, long long b)
{
	return __guest_syscall(n, a, b, 0, 0, 0, 0);
}

static __inline long __syscall3(long long n, long long a, long long b, long long c)
{
	return __guest_syscall(n, a, b, c, 0, 0, 0);
}

static __inline long __syscall4(long long n, long long a, long long b, long long c, long long d)
{
	return __guest_syscall(n, a, b, c, d, 0, 0);
}

static __inline long __syscall5(long long n, long long a, long long b, long long c, long long d, long long e)
{
	return __guest_syscall(n, a, b, c, d, e, 0);
}

static __inline long __syscall6(long long n, long long a, long long b, long long c, long long d, long long e, long long f)
{
	return __guest_syscall(n, a, b, c, d, e, f);
}

#define SYSCALL_NO_TLS 1
#define IPC_64 0
