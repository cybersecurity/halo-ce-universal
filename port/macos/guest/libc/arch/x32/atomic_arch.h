/* plain compiler atomics; the arch's inline assembly assumes 64-bit
pointers in its register operands */
#define a_ll a_ll
static inline int a_ll(volatile int *p)
{
	return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

#define a_cas a_cas
static inline int a_cas(volatile int *p, int t, int s)
{
	__atomic_compare_exchange_n(p, &t, s, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
	return t;
}

#define a_cas_p a_cas_p
static inline void *a_cas_p(volatile void *p, void *t, void *s)
{
	__atomic_compare_exchange_n((void *volatile *)p, &t, s, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
	return t;
}

#define a_swap a_swap
static inline int a_swap(volatile int *p, int v)
{
	return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST);
}

#define a_fetch_add a_fetch_add
static inline int a_fetch_add(volatile int *p, int v)
{
	return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST);
}

#define a_fetch_and a_fetch_and
static inline int a_fetch_and(volatile int *p, int v)
{
	return __atomic_fetch_and(p, v, __ATOMIC_SEQ_CST);
}

#define a_fetch_or a_fetch_or
static inline int a_fetch_or(volatile int *p, int v)
{
	return __atomic_fetch_or(p, v, __ATOMIC_SEQ_CST);
}

#define a_barrier a_barrier
static inline void a_barrier()
{
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
}

#define a_spin a_spin
static inline void a_spin()
{
	__builtin_ia32_pause();
	/* a compiler barrier, as the "memory" clobber of an asm yield would be */
	__atomic_signal_fence(__ATOMIC_SEQ_CST);
}

#define a_crash a_crash
static inline void a_crash()
{
	__builtin_trap();
}

#define a_ctz_64 a_ctz_64
static inline int a_ctz_64(uint64_t x)
{
	return __builtin_ctzll(x);
}

#define a_clz_64 a_clz_64
static inline int a_clz_64(uint64_t x)
{
	return __builtin_clzll(x);
}
