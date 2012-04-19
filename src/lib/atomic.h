/*
 * Copyright (c) 2011, Raphael Manfredi
 *
 *----------------------------------------------------------------------
 * This file is part of gtk-gnutella.
 *
 *  gtk-gnutella is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  gtk-gnutella is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with gtk-gnutella; if not, write to the Free Software
 *  Foundation, Inc.:
 *      59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *----------------------------------------------------------------------
 */

/**
 * @ingroup lib
 * @file
 *
 * Atomic memory operations.
 *
 * These a low-level operations that are required to implement spinlocks
 * and mutexes.
 *
 * @author Raphael Manfredi
 * @date 2011
 */

#ifndef _atomic_h_
#define _atomic_h_

#include "common.h"

/*
 * To avoid mistakes, the "volatile" attribute is made part of the typedef:
 * no access to the lock memory location should be optimized by the compiler,
 * or we might loop forever.
 */

typedef volatile uint8 atomic_lock_t;

/*
 * Public interface.
 */

#ifdef HAS_SYNC_ATOMIC
#define atomic_mb()					__sync_synchronize()
#define atomic_ops_available()		1

static inline ALWAYS_INLINE void
atomic_release(atomic_lock_t *p) {
	__sync_lock_release(p);
}

static inline ALWAYS_INLINE bool
atomic_test_and_set(atomic_lock_t *p)
{
	return __sync_bool_compare_and_swap(p, 0, 1);
}

static inline ALWAYS_INLINE void
atomic_int_inc(int *p)
{
	(void) __sync_fetch_and_add(p, 1);
}

static inline ALWAYS_INLINE void
atomic_uint_inc(unsigned *p)
{
	(void) __sync_fetch_and_add(p, 1);
}

static inline ALWAYS_INLINE bool
atomic_int_dec_is_zero(int *p)
{
	return 1 == __sync_fetch_and_sub(p, 1);
}

static inline ALWAYS_INLINE bool
atomic_uint_dec_is_zero(unsigned *p)
{
	return 1 == __sync_fetch_and_sub(p, 1);
}
#else	/* !HAS_SYNC_ATOMIC */
#define atomic_mb()					(void) 0

static inline bool
atomic_test_and_set(atomic_lock_t *p)
{
	int ok;
	if ((ok = (0 == *(p))))	
		*(p) = 1;
	return ok;
}

#define atomic_int_inc(p)			((*(p))++)
#define atomic_uint_inc(p)			((*(p))++)
#define atomic_int_dec_is_zero(p)	(0 == --(*(p)))
#define atomic_uint_dec_is_zero(p)	(0 == --(*(p)))
#define atomic_release(p)			(*(p) = 0)
#define atomic_ops_available()		0
#endif	/* HAS_SYNC_ATOMIC */

/**
 * Attempt to acquire the lock.
 *
 * @return TRUE if lock was acquired.
 */
static inline bool
atomic_acquire(atomic_lock_t *lock)
{
	/*
	 * Our locking protocol issues a memory barrier after a lock has been
	 * released, to make sure the changes to the locking object are widely
	 * visible to all processors.
	 *
	 * Therefore, it is not necessary to issue a memory barrier here.
	 */

	return atomic_test_and_set(lock);
}

#endif /* _atomic_h_ */

/* vi: set ts=4 sw=4 cindent: */
