/*
 * Copyright (c) 2017 Mindaugas Rasiukevicius <rmind at noxt eu>
 * All rights reserved.
 *
 * Use is subject to license terms, as specified in the LICENSE file.
 */

#ifndef	_UTILS_H_
#define	_UTILS_H_

#include <stddef.h>
#include <inttypes.h>
#include <limits.h>
#include <assert.h>

/*
 * A regular assert (debug/diagnostic only).
 */

#if defined(DEBUG)
#define	ASSERT		assert
#else
#define	ASSERT(x)
#endif

/*
 * Branch prediction macros.
 */

#ifndef __predict_true
#define	__predict_true(x)	__builtin_expect((x) != 0, 1)
#define	__predict_false(x)	__builtin_expect((x) != 0, 0)
#endif

/*
 * Minimum, maximum and rounding macros.
 */

#ifndef MIN
#define	MIN(x, y)	((x) < (y) ? (x) : (y))
#endif

#ifndef MAX
#define	MAX(x, y)	((x) > (y) ? (x) : (y))
#endif

#ifndef roundup2
#define	roundup2(x,m)	((((x) - 1) | ((m) - 1)) + 1)
#endif

/*
 * Counting the number of bits.
 */

#define	popcount32(x)		__builtin_popcount(x)

/*
 * Atomic operations and memory barriers.  If C11 API is not available,
 * then wrap the GCC builtin routines.
 */
#ifndef atomic_compare_exchange_weak
#define	atomic_compare_exchange_weak(ptr, expected, desired) \
    __sync_bool_compare_and_swap(ptr, expected, desired)
#endif

#ifndef atomic_exchange
static inline void *
atomic_exchange(volatile void *ptr, void *newval)
{
	void * volatile *ptrp = (void * volatile *)ptr;
	void *oldval;
again:
	oldval = *ptrp;
	if (!__sync_bool_compare_and_swap(ptrp, oldval, newval)) {
		goto again;
	}
	return oldval;
}
#endif

#ifndef atomic_thread_fence
#define	memory_order_acquire	__ATOMIC_ACQUIRE
#define	memory_order_release	__ATOMIC_RELEASE
#define	memory_order_seq_cst	__ATOMIC_SEQ_CST
#define	atomic_thread_fence(m)	__atomic_thread_fence(m)
#endif

/*
 * C11 memory model does not support classic load/store barriers.
 * Emulate it using the full memory barrier.
 */
#ifndef memory_order_loads
#define	memory_order_loads	memory_order_seq_cst
#endif
#ifndef memory_order_stores
#define	memory_order_stores	memory_order_seq_cst
#endif

/*
 * Exponential back-off for the spinning paths.
 */
#define	SPINLOCK_BACKOFF_MIN	4
#define	SPINLOCK_BACKOFF_MAX	128
#if defined(__x86_64__) || defined(__i386__)
#define SPINLOCK_BACKOFF_HOOK	__asm volatile("pause" ::: "memory")
#else
#define SPINLOCK_BACKOFF_HOOK
#endif
#define	SPINLOCK_BACKOFF(count)					\
do {								\
	int __i;						\
	for (__i = (count); __i != 0; __i--) {			\
		SPINLOCK_BACKOFF_HOOK;				\
	}							\
	if ((count) < SPINLOCK_BACKOFF_MAX)			\
		(count) += (count);				\
} while (/* CONSTCOND */ 0);

/*
 * Cache line size - a reasonable upper bound.
 */
#define	CACHE_LINE_SIZE		64

/*
 * Hash functions.
 */
uint32_t	murmurhash3(const void *, size_t, uint32_t);

#endif
