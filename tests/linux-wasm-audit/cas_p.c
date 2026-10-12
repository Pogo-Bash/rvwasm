/*
 * musl's arch/wasm/atomic_arch.h a_cas_p() as it is, against a pointer above 4 GiB and one between 2 and 4 GiB
 * (the values are only compared and stored, never dereferenced). Expect: the slot holds what was stored, and the
 * return value of a successful CAS is the old value. The proposed fix is checked the same way.
 */
#include <stdint.h>
#include <stdio.h>

#ifdef MUSL_ATOMIC
/* musl's own (build.sh with CFLAGS="-DMUSL_ATOMIC -I<musl source>/arch/wasm"). */
#include "atomic_arch.h"
#else
/* (copied from arch/wasm/atomic_arch.h) */
static inline int a_cas(volatile int *p, int t, int s)
{
	__atomic_compare_exchange_n(p, &t, s, (_Bool)0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
	return t;
}

static inline void *a_cas_p(volatile void *p, void *t, void *s)
{
	return (void *)a_cas((volatile int *)p, (long)t, (long)s);
}
#endif

/* The proposed fix: a compare-and-swap of the whole pointer. */
static inline void *a_cas_p_fixed(volatile void *p, void *t, void *s)
{
	__atomic_compare_exchange_n((void *volatile *)p, &t, s, (_Bool)0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
	return t;
}

static void *volatile slot;

int main(int argc, char **argv)
{
	void *high = (void *)0x100001000UL, *mid = (void *)0x80001000UL, *old;

	slot = 0;
	old = a_cas_p(&slot, 0, high);
	printf("cas(0 -> %p): returned %p, slot now %p (%s)\n", high, old, slot, slot == high ? "ok" : "WRONG: truncated");
	slot = mid;
	old = a_cas_p(&slot, mid, (void *)0x1000);
	printf("cas(%p -> 0x1000): returned %p (%s), slot now %p\n", mid, old, old == mid ? "ok" : "WRONG: sign-extended, the caller thinks the CAS failed", slot);
	slot = 0;
	old = a_cas_p_fixed(&slot, 0, high);
	printf("fixed: cas(0 -> %p): returned %p, slot now %p (%s)\n", high, old, slot, slot == high ? "ok" : "WRONG");
	slot = mid;
	old = a_cas_p_fixed(&slot, mid, (void *)0x1000);
	printf("fixed: cas(%p -> 0x1000): returned %p (%s)\n", mid, old, old == mid ? "ok" : "WRONG");
	return 0;
}
