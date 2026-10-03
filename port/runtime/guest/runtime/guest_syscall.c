/*
GUEST_SYSCALL.C

musl's system calls (arch/arm64_32/syscall_arch.h). The guest cannot enter
the kernel itself: the structures it would pass (timespec, stat, iovec) have
ILP32 layouts, and memory it maps must stay below 4 GB. The host performs
each call, converting where needed (host_syscall.c).
*/

#include "guest_host.h"

long __guest_syscall(long long number, long long a, long long b, long long c,
	long long d, long long e, long long f)
{
	return (long)host_syscall(number, a, b, c, d, e, f);
}
