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
 * Spinning locks.
 *
 * @author Raphael Manfredi
 * @date 2011
 */

#include "common.h"

#ifdef I_SCHED
#include <sched.h>
#endif

#define SPINLOCK_SOURCE

#include "spinlock.h"
#include "atomic.h"
#include "compat_sleep_ms.h"
#include "getcpucount.h"
#include "log.h"
#include "thread.h"
#include "tm.h"

#include "override.h"			/* Must be the last header included */

#define SPINLOCK_LOOP		100		/* Loop iterations before sleeping */
#define SPINLOCK_DELAY		2		/* Wait 2 ms before looping again */
#define SPINLOCK_DEAD		5000	/* # of loops before flagging deadlock */
#define SPINLOCK_TIMEOUT	20		/* Crash after 20 seconds */

static bool spinlock_pass_through;

static inline void
spinlock_account(const spinlock_t *s, const char *file, unsigned line)
{
	thread_lock_got(s, THREAD_LOCK_SPINLOCK, file, line, NULL);
}

static inline void
spinlock_account_swap(const spinlock_t *s, const char *file, unsigned line,
	const void *plock)
{
	thread_lock_got_swap(s, THREAD_LOCK_SPINLOCK, file, line, plock, NULL);
}

static inline void
spinunlock_account(const spinlock_t *s)
{
	thread_lock_released(s, THREAD_LOCK_SPINLOCK, NULL);
}

static inline void
spinlock_check(const volatile struct spinlock * const slock)
{
	g_assert(slock != NULL);
	g_assert(SPINLOCK_MAGIC == slock->magic);
}

/**
 * @return string for spinlock source.
 */
const char *
spinlock_source_string(enum spinlock_source src)
{
	switch (src) {
	case SPINLOCK_SRC_SPINLOCK:	return "spinlock";
	case SPINLOCK_SRC_MUTEX:	return "mutex";
	}
	g_assert_not_reached();
}

/**
 * Enter crash mode: let all spinlocks be grabbed immediately.
 */
G_GNUC_COLD void
spinlock_crash_mode(void)
{
	unsigned count = thread_count();

	if (count > 1 && !spinlock_pass_through) {
		s_minicrit("disabling locks, now in thread-unsafe mode (%u threads)",
			count);
	}

	spinlock_pass_through = TRUE;
}

/**
 * Warn about possible deadlock condition.
 *
 * Don't inline to provide a suitable breakpoint.
 */
static G_GNUC_COLD NO_INLINE void
spinlock_deadlock(const volatile void *obj, unsigned count)
{
	const volatile spinlock_t *s = obj;

	spinlock_check(s);

#ifdef SPINLOCK_DEBUG
	s_miniwarn("spinlock %p already held by %s:%u", obj, s->file, s->line);
#endif

	s_minicarp("possible spinlock deadlock #%u on %p", count, obj);
}

/**
 * Abort on deadlock condition.
 *
 * Don't inline to provide a suitable breakpoint.
 */
static G_GNUC_COLD NO_INLINE void G_GNUC_NORETURN
spinlock_deadlocked(const volatile void *obj, unsigned elapsed)
{
	const volatile spinlock_t *s = obj;
	static int deadlocked;

	if (deadlocked != 0) {
		if (1 == deadlocked)
			thread_lock_deadlock(obj);
		s_minierror("recursive deadlock on spinlock %p", obj);
	}

	deadlocked++;
	atomic_mb();

	spinlock_check(s);

#ifdef SPINLOCK_DEBUG
	s_miniwarn("spinlock %p still held by %s:%u", obj, s->file, s->line);
#endif

	thread_lock_deadlock(obj);
	s_error("deadlocked on spinlock %p (after %u secs)", obj, elapsed);
}

/**
 * Obtain a lock, spinning first then spleeping.
 *
 * The routine does not return unless the lock is acquired.  When waiting
 * for too long, we first warn about possible deadlocks, then force a deadlock
 * condition after more time.  The supplied callbacks are there to perform
 * the proper logging based on the source object being locked (and not on
 * the spinlock itself which may be part of a more complex lock, like a mutex).
 *
 * No accounting of the lock is made, this must be handled by the caller
 * upon return.
 *
 * @param s				the spinlock we're trying to acquire
 * @param src			the type of object containing the spinlock
 * @param src_object	the lock object containing the spinlock
 * @param deadlock		callback to invoke when we detect a possible deadlock
 * @param deadlocked	callback to invoke when we decide we deadlocked
 */
void
spinlock_loop(volatile spinlock_t *s,
	enum spinlock_source src, const volatile void *src_object,
	spinlock_deadlock_cb_t deadlock, spinlock_deadlocked_cb_t deadlocked)
{
	static long cpus;
	unsigned i;
	time_t start = 0;
	int loops = SPINLOCK_LOOP;

	spinlock_check(s);

	/*
	 * This routine is only called when there is a lock contention, and
	 * therefore it is not on the fast locking path.  We can therefore
	 * afford to conduct more extended checks.
	 */

	if G_UNLIKELY(0 == cpus)
		cpus = getcpucount();

	/*
	 * If in "pass-through" mode, we're crashing, so avoid deadlocks.
	 */

	if G_UNLIKELY(spinlock_pass_through) {
		spinlock_direct(s);
		return;
	}

	/*
	 * When running mono-threaded, having to loop means we're deadlocked
	 * already, so immediately flag it.
	 */

	if (thread_is_single())
		(*deadlocked)(src_object, 0);

	/*
	 * If the thread already holds the lock object, we're deadlocked.
	 *
	 * We don't need to check that for mutexes because we would not get here
	 * for a mutex already held by the thread: this is not a contention case.
	 */

	if (SPINLOCK_SRC_SPINLOCK == src && thread_lock_holds(src_object))
		(*deadlocked)(src_object, 0);

#ifdef HAS_SCHED_YIELD
	if (1 == cpus)
		loops /= 10;
#endif

	for (i = 0; /* empty */; i++) {
		int j;

		for (j = 0; j < loops; j++) {
			if G_UNLIKELY(SPINLOCK_MAGIC != s->magic) {
				s_error("spinlock %s whilst waiting on %s %p, at attempt #%u",
					SPINLOCK_DESTROYED == s->magic ? "destroyed" : "corrupted",
					spinlock_source_string(src), src_object, i);
			}

			if (s->lock) {
				/* Lock is busy, do nothing as cheaply as possible */
			} else if (atomic_acquire(&s->lock)) {
#ifdef SPINLOCK_DEBUG
				if (i >= SPINLOCK_DEAD) {
					s_miniinfo("finally grabbed %s %p after %u attempts",
						spinlock_source_string(src), src_object, i);
				}
#endif	/* SPINLOCK_DEBUG */
				return;
			}
#ifdef HAS_SCHED_YIELD
			if (1 == cpus)
				do_sched_yield();		/* See lib/mingw32.h */
#endif
		}

		if G_UNLIKELY(i != 0 && 0 == i % SPINLOCK_DEAD)
			(*deadlock)(src_object, i / SPINLOCK_DEAD);

		if G_UNLIKELY(0 == start)
			start = tm_time_exact();

		if (delta_time(tm_time_exact(), start) > SPINLOCK_TIMEOUT)
			(*deadlocked)(src_object, (unsigned) delta_time(tm_time(), start));

		compat_sleep_ms(SPINLOCK_DELAY);
	}
}

/**
 * Initialize a non-static spinlock.
 */
void
spinlock_init(spinlock_t *s)
{
	g_assert(s != NULL);

	s->magic = SPINLOCK_MAGIC;
	s->lock = 0;
#ifdef SPINLOCK_DEBUG
	s->file = NULL;
	s->line = 0;
#endif
	atomic_mb();
}

/**
 * Destroy a spinlock.
 *
 * It is not necessary to hold the lock on the spinlock to do this, although
 * one must be careful to not destroy a spinlock that could be used by another
 * thread.
 *
 * If not already locked, the spinlock is grabbed before being destroyed to
 * make sure nobody attempts to grab it whilst we're invalidating it.
 *
 * Any further attempt to use this spinlock will cause an assertion failure.
 */
void
spinlock_destroy(spinlock_t *s)
{
	bool was_locked;

	spinlock_check(s);

	if (atomic_acquire(&s->lock)) {
		g_assert(SPINLOCK_MAGIC == s->magic);
		was_locked = FALSE;
	} else {
		was_locked = TRUE;
	}

	s->magic = SPINLOCK_DESTROYED;		/* Now invalid */
	atomic_mb();

	/*
	 * The normal protocol is to spinlock() before destroying.  If the lock
	 * was held on entry, we have to assume it was locked by the thread.
	 * Otherwise, we have an error condition anyway (destroying a lock not
	 * taken by the thread).
	 */

	if (was_locked)
		spinunlock_account(s);
}

/**
 * Grab a spinlock from said location.
 */
void
spinlock_grab_from(spinlock_t *s, bool hidden, const char *file, unsigned line)
{
	spinlock_check(s);

	if G_UNLIKELY(!atomic_acquire(&s->lock)) {
		spinlock_loop(s, SPINLOCK_SRC_SPINLOCK, s,
			spinlock_deadlock, spinlock_deadlocked);
	}

	s->file = file;
	s->line = line;

	if G_LIKELY(!hidden)
		spinlock_account(s, file, line);
}

/**
 * Grab spinlock from said location, only if available.
 *
 * @return whether we obtained the lock.
 */
bool
spinlock_grab_try_from(spinlock_t *s,
	bool hidden, const char *file, unsigned line)
{
	spinlock_check(s);

	if (atomic_acquire(&s->lock)) {
		s->file = file;
		s->line = line;
		if G_LIKELY(!hidden)
			spinlock_account(s, file, line);
		return TRUE;
	}

	if G_UNLIKELY(spinlock_pass_through)
		return TRUE;		/* Crashing */

	return FALSE;
}

/**
 * Grab regular spinlock, exchanging lock position with previous lock.
 */
void
spinlock_grab_swap_from(spinlock_t *s, const void *plock,
	const char *file, unsigned line)
{
	spinlock_check(s);

	if G_UNLIKELY(!atomic_acquire(&s->lock)) {
		spinlock_loop(s, SPINLOCK_SRC_SPINLOCK, s,
			spinlock_deadlock, spinlock_deadlocked);
	}

	s->file = file;
	s->line = line;

	spinlock_account_swap(s, file, line, plock);
}

/**
 * Attempt to grab regular spinlock, exchanging lock position with previous
 * lock.
 *
 * @return whether we obtained the lock.
 */
bool
spinlock_grab_swap_try_from(spinlock_t *s, const void *plock,
	const char *file, unsigned line)
{
	spinlock_check(s);

	if (atomic_acquire(&s->lock)) {
		s->file = file;
		s->line = line;
		spinlock_account_swap(s, file, line, plock);
		return TRUE;
	}

	if G_UNLIKELY(spinlock_pass_through)
		return TRUE;		/* Crashing */

	return FALSE;
}

/**
 * Release spinlock, which must be locked currently.
 */
void
spinlock_release(spinlock_t *s, bool hidden)
{
	spinlock_check(s);
	g_assert(s->lock != 0 || spinlock_pass_through);

	/*
	 * The release acts as a "release barrier", ensuring that all previous
	 * stores have been made globally visible in memory.
	 */

	atomic_release(&s->lock);

	if G_LIKELY(!hidden)
		spinunlock_account(s);
}

/* vi: set ts=4 sw=4 cindent: */
