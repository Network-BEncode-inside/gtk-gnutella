/*
 * Copyright (c) 2003-2005, Raphael Manfredi
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
 * Time manipulation and caching routines.
 *
 * @author Raphael Manfredi
 * @date 2003-2005
 */

#include "common.h"

#ifdef I_SYS_TIMES
#include <sys/times.h>
#endif
#ifdef I_SYS_SELECT
#include <sys/select.h>		/* For "struct timeval" on some systems */
#endif

#include "tm.h"
#include "atomic.h"
#include "compat_sleep_ms.h"
#include "listener.h"
#include "offtime.h"
#include "spinlock.h"
#include "thread.h"

#include "override.h"		/* Must be the last header included */

#define TM_GMT_PERIOD		(30*60)	/* Recompute GMT offset every half hour */
#define TM_THREAD_STACK		THREAD_STACK_MIN
#define TM_THREAD_PERIOD	1000	/* ms, 1 second */

tm_t tm_cached_now;			/* Currently cached time */
static spinlock_t tm_slk = SPINLOCK_INIT;

static struct {
	time_delta_t offset;
	time_t computed;
} tm_gmt;

static bool tm_thread_started;

#define TM_LOCK			spinlock_raw(&tm_slk)
#define TM_UNLOCK		spinunlock_raw(&tm_slk)

/**
 * Clock update listerners.
 */

static listeners_t tm_event_listeners;

void tm_event_listener_add(tm_event_listener_t l)
{
	LISTENER_ADD(tm_event, l);
}

void tm_event_listener_remove(tm_event_listener_t l)
{
	LISTENER_REMOVE(tm_event, l);
}

static void
tm_event_fire(int delta)
{
	LISTENER_EMIT(tm_event, (delta));
}

/**
 * Get current time for the system, filling the supplied tm_t structure.
 */
static void
tm_current_time(tm_t *tm)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	tm->tv_sec = tv.tv_sec;
	tm->tv_usec = tv.tv_usec;
}

/*
 * Recompute the GMT offset.
 */
static void
tm_update_gmt_offset(const time_t now)
{
	time_delta_t gmtoff;
	struct tm tp;
	
	/*
	 * The ``tm_gmt'' variable is only updated from the time thread, hence
	 * there is no need to lock it.
	 */

	gmtoff = timestamp_gmt_offset(now, NULL);
	tm_gmt.offset = gmtoff;
	atomic_mb();				/* Make all threads aware of the change */

	/*
	 * Recompute the time at which we'll need to check for a DST change.
	 * Daylight Saving Time changes only occur at the hour or half hour,
	 * local time.  Hence we force the date at which we computed the GMT
	 * offset to the beginning of the hour or of the half-hour.
	 */

	off_time(now, gmtoff, &tp);
	tp.tm_min = (tp.tm_min >= 30) ? 30 : 0;		/* Last start or half hour */
	tm_gmt.computed = mktime(&tp);
}

/**
 * Called when time has been updated by the time thread, normally every second.
 *
 * @return whether time variation occurred.
 */
static bool
tm_updated(const tm_t *prev, const tm_t *now)
{
	time_delta_t delta;

	/*
	 * Periodically update the GMT offset.
	 */

	if G_UNLIKELY(delta_time(now->tv_sec, tm_gmt.computed) > TM_GMT_PERIOD)
		tm_update_gmt_offset((time_t) now->tv_sec);

	/*
	 * When time is shifting suddenly (system-wide time adjustment, either
	 * from the super-user or from NTP), and especially when it is moving
	 * backwards, we need to react to avoid problems:
	 *
	 * - Code waiting on condition variables with a timeout need to recompute
	 *   the proper time to avoid being stuck for longer than necessary if
	 *   time is moving backwards.
	 *
	 * - Code monitoring the time spent, such as spinlocks, need to be warned
	 *   so that they do not trigger the timeout condition when time jumps
	 *   forward.  These should use gentime_difftime() to make sure the delta
	 *   remains accurate in that case and gentime_now() to get the current
	 *   time.
	 *
	 * Code is responsible for registering the appropriate listeners to react
	 * when we detect a variation in the system's clock.
	 */

	if G_UNLIKELY(0 == prev->tv_sec)
		return FALSE;

	delta = tm_elapsed_ms(now, prev) - TM_THREAD_PERIOD;

	if G_LIKELY(delta >= -TM_THREAD_PERIOD/4 && delta <= TM_THREAD_PERIOD/4)
		return FALSE;

	tm_update_gmt_offset((time_t) now->tv_sec);
	tm_event_fire(delta);

	return TRUE;
}

/**
 * Time thread.
 *
 * This is launched to update the time every second, check whether the
 * system clock is moving ahead/backwards and update our GMT offset
 * regularily.
 */
static void *
tm_thread_main(void *unused_arg)
{
	tm_t prev;

	(void) unused_arg;
	ZERO(&prev);

	thread_set_name("time");

	for (;;) {
		tm_t now;

		G_PREFETCH_HI_R(&tm_gmt.computed);

		TM_LOCK;
		tm_current_time(&tm_cached_now);
		now = tm_cached_now;
		TM_UNLOCK;

		if G_UNLIKELY(tm_updated(&prev, &now)) {
			/*
			 * Updating could take some time, so we need to refresh the
			 * previous time.  If the system clock is updated whilst
			 * being in tm_updated() and we detected a time shift already,
			 * then we won't be able to see this second update but the
			 * chances of that happening are slim.
			 */
			tm_current_time(&prev);
		} else {
			prev = now;
		}
		compat_sleep_ms(TM_THREAD_PERIOD);
	}

	g_assert_not_reached();
	return NULL;
}

/**
 * Start time thread, once.
 */
static void
tm_thread_start(void)
{
	int r;

	r = thread_create(tm_thread_main, NULL, THREAD_F_DETACH, TM_THREAD_STACK);
	if G_UNLIKELY(-1 == r)
		s_minierror("%s(): cannot launch time thread: %m", G_STRFUNC);
}

/**
 * Convert floating point time description into a struct timeval by filling
 * in the supplied structure.
 */
void
f2tm(double t, tm_t *tm)
{
	tm->tv_sec = (unsigned long) t;
	tm->tv_usec = (long) ((t - (double) tm->tv_sec) * 1000000.0);
}

/**
 * Computes the elapsed time (t1 - t0) in the supplied structure.
 */
void
tm_elapsed(tm_t *elapsed, const tm_t *t1, const tm_t *t0)
{
	elapsed->tv_sec = t1->tv_sec - t0->tv_sec;
	elapsed->tv_usec = t1->tv_usec - t0->tv_usec;
	if (elapsed->tv_usec < 0) {
		elapsed->tv_usec += 1000000;
		elapsed->tv_sec--;
	}
}

/**
 * In-place substract dec from tm.
 */
void
tm_sub(tm_t *tm, const tm_t *dec)
{
	tm->tv_sec -= dec->tv_sec;
	tm->tv_usec -= dec->tv_usec;
	if (tm->tv_usec < 0) {
		tm->tv_usec += 1000000;
		tm->tv_sec--;
	}
}

/**
 * In-place add inc to tm.
 */
void
tm_add(tm_t *tm, const tm_t *inc)
{
	tm->tv_sec += inc->tv_sec;
	tm->tv_usec += inc->tv_usec;
	if (tm->tv_usec >= 1000000) {
		tm->tv_usec -= 1000000;
		tm->tv_sec++;
	}
}

/**
 * Compare two times and return -1, 0 or +1 depending on their relative order.
 */
int
tm_cmp(const tm_t *a, const tm_t *b)
{
	if (a->tv_sec != b->tv_sec)
		return (a->tv_sec > b->tv_sec) ? +1 : -1;
	if (a->tv_usec == b->tv_usec)
		return 0;
	return (a->tv_usec > b->tv_usec) ? +1 : -1;
}

/**
 * Computes the remaining time to absolute end time and return duration
 * in milliseconds.
 *
 * This routine is more accurate than tm_elapsed_ms() because it goes down
 * to the microsecond in case there are no visible difference at the
 * millisecond level.
 *
 * @param end		absolute ending time
 *
 * @return amount of milliseconds remaining to reach time.
 */
long
tm_remaining_ms(const tm_t *end)
{
	tm_t now, elapsed;
	long remain;

	tm_now_exact(&now);
	tm_elapsed(&elapsed, end, &now);
	remain = tm2ms(&elapsed);

	/*
	 * We want the full precision, so if remain is 0, go down to the
	 * micro-second level to check whether waiting really expired.
	 */

	if G_UNLIKELY(0 == remain) {
		long us = tm2us(&elapsed);
		if (us < 0)
			remain = -1;		/* Signal that we're past the time */
		else if (us > 0)
			remain = 1;			/* Signal that we're before the time */
	}

	return remain;
}

/**
 * Fill supplied structure with current time (cached).
 */
void
tm_now(tm_t *tm)
{
	if G_UNLIKELY(thread_check_suspended()) {
		tm_now_exact(tm);
	} else {
		TM_LOCK;
		*tm = tm_cached_now;	/* Struct copy */
		TM_UNLOCK;
	}
}

/**
 * Fill supplied structure with current time (recomputed).
 */
void
tm_now_exact(tm_t *tm)
{
	G_PREFETCH_HI_W(&tm_cached_now);
	G_PREFETCH_HI_W(&tm_slk);

	/*
	 * This routine is too low-level to be able to use once_run_flag().
	 */

	if G_UNLIKELY(!tm_thread_started) {
		bool start = FALSE;
		TM_LOCK;
		if (!tm_thread_started)
			start = tm_thread_started = TRUE;
		TM_UNLOCK;
		if (start)
			tm_thread_start();
	}

	thread_check_suspended();

	TM_LOCK;
	tm_current_time(&tm_cached_now);
	if G_LIKELY(tm != NULL)
		*tm = tm_cached_now;
	TM_UNLOCK;
}

/**
 * Get current time, at the second granularity (recomputed).
 */
time_t
tm_time_exact(void)
{
	tm_now_exact(NULL);
	return (time_t) tm_cached_now.tv_sec;
}

/**
 * Get current local time, at the second granularity (cached).
 */
time_t
tm_localtime(void)
{
	G_PREFETCH_HI_R(&tm_cached_now);
	G_PREFETCH_HI_R(&tm_gmt.offset);

	if G_UNLIKELY(thread_check_suspended()) {
		return tm_localtime_exact();
	} else {
		return (time_t) tm_cached_now.tv_sec + tm_gmt.offset;
	}
}

/**
 * Get current local time, at the second granularity (recomputed).
 */
time_t
tm_localtime_exact(void)
{
	tm_now_exact(NULL);
	return (time_t) tm_cached_now.tv_sec + tm_gmt.offset;
}

/**
 * Hash a tm_t time structure.
 */
uint
tm_hash(const void *key)
{
	const tm_t *tm = key;

	return tm->tv_sec ^ (tm->tv_usec << 10) ^ (tm->tv_usec & 0x3ff);
}

/**
 * Test two tm_t for equality.
 */
int
tm_equal(const void *a, const void *b)
{
	const tm_t *ta = a, *tb = b;

	return ta->tv_sec == tb->tv_sec && ta->tv_usec == tb->tv_usec;
}

/***
 *** CPU time computation.
 ***/

#ifdef HAS_TIMES
/**
 * Return amount of clock ticks per second.
 */
static long 
clock_hz(void)
{
	static long freq = 0;	/* Cached amount of clock ticks per second */

	if G_UNLIKELY(freq <= 0) {
#ifdef _SC_CLK_TCK
		errno = ENOTSUP;
		freq = sysconf(_SC_CLK_TCK);
		if (-1L == freq)
			g_warning("sysconf(_SC_CLK_TCK) failed: %m");
#endif
	}

	if G_UNLIKELY(freq <= 0) {
#if defined(CLK_TCK)
		freq = CLK_TCK;			/* From <time.h> */
#elif defined(HZ)
		freq = HZ;				/* From <sys/param.h> ususally */
#elif defined(CLOCKS_PER_SEC)
		/* This is actually for clock() but should be OK. */
		freq = CLOCKS_PER_SEC;	/* From <time.h> */
#else
		freq = 1;
#error	"unable to determine clock frequency base"
#endif
	}

	return freq;
}
#endif	/* HAS_TIMES */

/**
 * Fill supplied variables with CPU usage time (user and kernel), if not NULL.
 *
 * @return total CPU time used so far (user + kernel).
 */
double
tm_cputime(double *user, double *sys)
{
	static bool getrusage_failed;
	double u;
	double s;

	if (!getrusage_failed) {
#if defined(HAS_GETRUSAGE)
		struct rusage usage;

		errno = ENOTSUP;
		if G_UNLIKELY(-1 == getrusage(RUSAGE_SELF, &usage)) {
			u = 0;
			s = 0;
			g_warning("getrusage(RUSAGE_SELF, ...) failed: %m");
		} else {
			u = tm2f(timeval_to_tm(&usage.ru_utime));
			s = tm2f(timeval_to_tm(&usage.ru_stime));
		}
#else
		getrusage_failed = TRUE;
#endif /* HAS_GETRUSAGE */
	} else {
		/* For stupid compilers */
		u = 0;
		s = 0;
	}

	if (getrusage_failed) {	
#if defined(HAS_TIMES)
		struct tms t;

		(void) times(&t);

		u = (double) t.tms_utime / (double) clock_hz();
		s = (double) t.tms_stime / (double) clock_hz();
#else
		static bool warned = FALSE;

		if (!warned) {
			g_warning("getrusage() is unusable and times() is missing");
			g_warning("will be unable to monitor CPU usage; using wall clock.");
			warned = TRUE;
		}

		u = (double) tm_time_exact();	/* Wall clock */
		s = 0.0;						/* We have no way of knowing that */
#endif	/* HAS_TIMES */
	}

	if (user) *user = u;
	if (sys)  *sys  = s;

	return u + s;
}

static tm_t start_time;

void
tm_init(void)
{
	tm_now_exact(&start_time);
}

tm_t
tm_start_time(void)
{
	return start_time;
}

/* vi: set ts=4 sw=4 cindent: */
