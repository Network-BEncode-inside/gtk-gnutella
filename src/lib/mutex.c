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
 * Mutual thread exclusion locks.
 *
 * @author Raphael Manfredi
 * @date 2011
 */

#include "common.h"

#define MUTEX_SOURCE

#include "mutex.h"
#include "atomic.h"
#include "log.h"
#include "spinlock.h"
#include "thread.h"
#include "tm.h"

#include "override.h"			/* Must be the last header included */

static bool mutex_pass_through;

static inline void
mutex_get_account(const mutex_t *m)
{
	thread_lock_got(m, THREAD_LOCK_MUTEX);
}

static inline void
mutex_release_account(const mutex_t *m)
{
	thread_lock_released(m, THREAD_LOCK_MUTEX);
}

static inline void
mutex_check(const volatile struct mutex * const mutex)
{
	g_assert(mutex != NULL);
	g_assert(MUTEX_MAGIC == mutex->magic);
}

/**
 * Enter crash mode: allow all mutexes to be silently released.
 */
G_GNUC_COLD void
mutex_crash_mode(void)
{
	mutex_pass_through = TRUE;
}

/**
 * Warn about possible deadlock condition.
 *
 * Don't inline to provide a suitable breakpoint.
 */
static G_GNUC_COLD NO_INLINE void
mutex_deadlock(const volatile void *obj, unsigned count)
{
	const volatile mutex_t *m = obj;

	mutex_check(m);

#ifdef SPINLOCK_DEBUG
	s_miniwarn("mutex %p already held (depth %zu) by %s:%u",
		obj, m->depth, m->lock.file, m->lock.line);
#endif

	s_minicarp("possible mutex deadlock #%u on %p", count, obj);
}

/**
 * Abort on deadlock condition.
 *
 * Don't inline to provide a suitable breakpoint.
 */
static G_GNUC_COLD NO_INLINE void G_GNUC_NORETURN
mutex_deadlocked(const volatile void *obj, unsigned elapsed)
{
	const volatile mutex_t *m = obj;
	static int deadlocked;

	if (deadlocked != 0) {
		if (1 == deadlocked)
			thread_lock_deadlock(obj);
		s_minierror("recursive deadlock on mutex %p (depth %zu)",
			obj, m->depth);
	}

	deadlocked++;
	atomic_mb();

	mutex_check(m);

#ifdef SPINLOCK_DEBUG
	s_miniwarn("mutex %p still held (depth %zu) by %s:%u",
		obj, m->depth, m->lock.file, m->lock.line);
#endif

	thread_lock_deadlock(obj);
	s_error("deadlocked on mutex %p (depth %zu, after %u secs)",
		obj, m->depth, elapsed);
}

/**
 * Initialize a non-static mutex.
 */
void
mutex_init(mutex_t *m)
{
	g_assert(m != NULL);

	m->magic = MUTEX_MAGIC;
	m->owner = 0;
	m->depth = 0;
	spinlock_init(&m->lock);	/* Issues the memory barrier */
}

/**
 * Is mutex owned by thread?
 */
static inline ALWAYS_INLINE bool
mutex_is_owned_by_fast(const mutex_t *m, const thread_t t)
{
	return spinlock_is_held(&m->lock) && thread_eq(t, m->owner);
}

/**
 * Is mutex owned by thread?
 */
bool
mutex_is_owned_by(const mutex_t *m, const thread_t t)
{
	mutex_check(m);

	return mutex_is_owned_by_fast(m, t);
}

/**
 * Is mutex owned?
 */
bool
mutex_is_owned(const mutex_t *m)
{
	return mutex_is_owned_by(m, thread_current());
}

/**
 * Destroy a mutex.
 *
 * It is not necessary to hold the lock on the mutex to do this, although
 * one must be careful to not destroy a mutex that could be used by another
 * thread.
 *
 * If not already locked, the mutex is grabbed before being destroyed to
 * make sure nobody attempts to grab it whilst we're invalidating it.
 *
 * Any further attempt to use this mutex will cause an assertion failure.
 */
void
mutex_destroy(mutex_t *m)
{
	mutex_check(m);

	if (spinlock_hidden_try(&m->lock) || mutex_is_owned(m)) {
		g_assert(MUTEX_MAGIC == m->magic);
	}

	m->magic = MUTEX_DESTROYED;		/* Now invalid */
	m->owner = 0;
	spinlock_destroy(&m->lock);		/* Issues the memory barrier */
}

/**
 * Grab a mutex.
 */
void
mutex_grab(mutex_t *m, bool hidden)
{
	mutex_check(m);
	thread_t t = thread_current();

	/*
	 * We dispense with memory barriers after getting the spinlock because
	 * the atomic test-and-set instruction should act as an acquire barrier,
	 * meaning that anything we write after the lock cannot be moved before
	 * by the memory logic.
	 *
	 * We check for a recursive grabbing of the mutex first because this is
	 * a cheap test to perform, then we attempt the atomic operations to
	 * actually grab it.
	 */

	if (mutex_is_owned_by_fast(m, t)) {
		m->depth++;
	} else if (spinlock_hidden_try(&m->lock)) {
		thread_set(m->owner, t);
		m->depth = 1;
	} else {
		spinlock_loop(&m->lock, SPINLOCK_SRC_MUTEX, m,
			mutex_deadlock, mutex_deadlocked);
		thread_set(m->owner, t);
		m->depth = 1;
	}

	if G_LIKELY(!hidden)
		mutex_get_account(m);
}

/**
 * Grab mutex only if available.
 *
 * @return whether we obtained the mutex.
 */
bool
mutex_grab_try(mutex_t *m)
{
	mutex_check(m);
	thread_t t = thread_current();

	if (spinlock_hidden_try(&m->lock)) {
		thread_set(m->owner, t);
		m->depth = 1;
	} else if (mutex_is_owned_by(m, t)) {
		m->depth++;
	} else {
		return FALSE;
	}

	mutex_get_account(m);
	return TRUE;
}

#ifdef SPINLOCK_DEBUG
/**
 * Grab a mutex from said location.
 */
void
mutex_grab_from(mutex_t *m, bool hidden, const char *file, unsigned line)
{
	mutex_grab(m, hidden);

	if (1 == m->depth) {
		m->lock.file = file;
		m->lock.line = line;
	}
}

/**
 * Grab mutex from said location, only if available.
 *
 * @return whether we obtained the mutex.
 */
bool
mutex_grab_try_from(mutex_t *m, const char *file, unsigned line)
{
	if (mutex_grab_try(m)) {
		if (1 == m->depth) {
			m->lock.file = file;
			m->lock.line = line;
		}
		return TRUE;
	}

	return FALSE;
}
#endif	/* SPINLOCK_DEBUG */

/**
 * Release a mutex, which must be owned currently.
 */
void
mutex_ungrab(mutex_t *m, bool hidden)
{
	mutex_check(m);

	/*
	 * We don't immediately assert that the mutex is owned to not penalize
	 * the regular path, and to cleanly cut through the assertion when we're
	 * in crash mode.
	 */

	if G_UNLIKELY(!mutex_is_owned(m)) {	/* Precondition */
		if (mutex_pass_through)
			return;
		/* OK, re-assert so that we get the precondition failure */
		g_assert_log(mutex_is_owned(m),
			"attempt to release unowned mutex %p (depth=%zu, owner=thread #%d)",
			m, m->depth, thread_stid_from_thread(m->owner));
	}

	if (0 == --m->depth) {
		m->owner = 0;
		spinunlock_hidden(&m->lock);	/* Acts as a "release barrier" */
	}

	if G_LIKELY(!hidden)
		mutex_release_account(m);
}

/**
 * Convenience routine for locks that are part of a "const" structure.
 */
void
mutex_release_const(const mutex_t *m)
{
	/*
	 * A lock is not part of the abstract data type, so it's OK to
	 * de-constify it now: no mutex is really read-only.
	 */

	mutex_ungrab(deconstify_pointer(m), FALSE);
}

/**
 * Check whether someone holds the mutex and at which depth.
 *
 * @return the depth at which the mutex belongs to a thread.
 */
size_t
mutex_held_depth(const mutex_t *m)
{
	mutex_check(m);

	return spinlock_is_held(&m->lock) ? m->depth : 0;
}

/* vi: set ts=4 sw=4 cindent: */
