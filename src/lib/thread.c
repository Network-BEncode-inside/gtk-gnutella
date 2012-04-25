/*
 * Copyright (c) 2011-2012 Raphael Manfredi
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
 * Minimal thread support.
 *
 * This mainly provides support for thread-private data, as well as minimal
 * thread tracking.
 *
 * It works by cooperation with the spinlock/mutex code that we're using,
 * providing hooks so that can detect the existence of new threads on the
 * fly and track them.
 *
 * We are not interested by threads that could exist out there and which never
 * enter our code somehow, either through a lock (possibly indirectly by
 * calling a memory allocation routine) or through logging.
 *
 * @author Raphael Manfredi
 * @date 2011-2012
 */

#include "common.h"

#define THREAD_SOURCE			/* We want hash_table_new_real() */

#include "thread.h"
#include "alloca.h"				/* For alloca_stack_direction() */
#include "compat_sleep_ms.h"
#include "crash.h"				/* For print_str() et al. */
#include "hashing.h"			/* For binary_hash() */
#include "hashtable.h"
#include "mutex.h"
#include "omalloc.h"
#include "pow2.h"
#include "spinlock.h"
#include "stacktrace.h"
#include "stringify.h"
#include "vmm.h"
#include "zalloc.h"

#include "override.h"			/* Must be the last header included */

/**
 * To quickly access thread-private data, we introduce the notion of Quasi
 * Thread Ids, or QIDs: they are not unique for a given thread but no two
 * threads can have the same QID at a given time.
 */
#define THREAD_QID_BITS		8		/**< QID bits used for hashing */
#define THREAD_QID_CACHE	(1U << THREAD_QID_BITS)	/**< QID cache size */

#define THREAD_MAX			64		/**< Max amount of threads we can track */
#define THREAD_LOCK_MAX		256		/**< Max amount of locks held */

#define THREAD_SUSPEND_CHECK	5000
#define THREAD_SUSPEND_TIMEOUT	30	/* seconds */

/**
 * A recorded lock.
 */
struct thread_lock {
	const void *lock;				/**< Lock object address */
	enum thread_lock_kind kind;		/**< Kind of lock recorded */
};

/*
 * A lock stack.
 */
struct thread_lock_stack {
	struct thread_lock *arena;		/**< The actual stack */
	size_t capacity;				/**< Amount of entries available */
	size_t count;					/**< Amount of entries held */
	uint8 overflow;					/**< Set if stack overflow detected */
};

/**
 * A thread-private value.
 */
struct thread_pvalue {
	void *value;					/**< The actual value */
	thread_pvalue_free_t p_free;	/**< Optional free routine */
	void *p_arg;					/**< Optional argument to free routine */
};

/**
 * A thread element, describing a thread.
 */
struct thread_element {
	thread_t tid;					/**< The thread ID */
	thread_qid_t last_qid;			/**< The last QID used to access record */
	hash_table_t *pht;				/**< Private hash table */
	unsigned stid;					/**< Small thread ID */
	const void *stack_base;			/**< Plausible stack base */
	int suspend;					/**< Suspension request(s) */
	int pending;					/**< Pending messages to emit */
	uint deadlocked:1;				/**< Whether thread reported deadlock */
	struct thread_lock_stack locks;	/**< Locks held by thread */
};

/**
 * Private zones.
 *
 * We use raw zalloc() instead of walloc() to minimize the amount of layers
 * upon which this low-level service depends.
 *
 * Furthermore, each zone is allocated as an embedded item to avoid any
 * allocation via xmalloc(): it solely depends on the VMM layer, the zone
 * descriptor being held at the head of the first zone arena.
 */
static zone_t *pvzone;		/* For private values */

/**
 * Array of threads, by small thread ID.
 */
static struct thread_element *threads[THREAD_MAX];

/**
 * This array is used solely during creation of a new thread element.
 *
 * Its purpose is to be able to return a thread small ID whilst we are in
 * the process of creating that thread element, for instance if we have to
 * call a logging routine as part of the thread creation.
 */
static thread_t tstid[THREAD_MAX];

/**
 * QID cache.
 *
 * This is an array indexed by a hashed QID and it enables fast access to a
 * thread element, without locking.
 *
 * The method used is the following: the QID is computed for the thread and
 * then the cache is accessed to see which thread element it refers to.  If an
 * entry is found, its last_qid field is compared to the current QID and if it
 * matches, then we found the item we were looking for.
 *
 * Otherwise (no entry in the cache or the last_qid does not match), a full
 * lookup through the global hash table is performed to locate the item, and
 * it is inserted in the cache.
 */
static struct thread_element *thread_qid_cache[THREAD_QID_CACHE];
static uint8 thread_qid_busy[THREAD_QID_CACHE];

static bool thread_inited;
static int thread_pageshift = 12;		/* Safe default: 4K pages */
static int thread_sp_direction;			/* Stack growth direction */

static spinlock_t thread_private_slk = SPINLOCK_INIT;
static spinlock_t thread_insert_slk = SPINLOCK_INIT;
static mutex_t thread_suspend_mtx = MUTEX_INIT;

#ifdef I_PTHREAD
/**
 * Low-level unique thread ID.
 */
static inline thread_t
thread_self(void)
{
	union {
		thread_t t;
		pthread_t pt;
	} u;

	STATIC_ASSERT(sizeof(thread_t) <= sizeof(pthread_t));

	/*
	 * We truncate the pthread_t to the first "unsigned long" bytes.
	 *
	 * On Linux, pthread_t is already an unsigned long.
	 * On FreeBSD, pthread_t is a pointer, which fits in unsigned long.
	 *
	 * On Windows, pthread_t is a structure, whose first member is a pointer.
	 * And we don't want to use the whole pthread_t structure there, because
	 * the second member is changing over time and we want a unique thread
	 * identifier.
	 */

	u.pt = pthread_self();
	return u.t;
}
#else
#define thread_self()   0xc5db8dd3UL	/* Random, odd number */
#endif	/* I_PTHREAD */

/**
 * Compare two stack pointers according to the stack growth direction.
 * A pointer is larger than another if it is further away from the base.
 */
static inline int
thread_stack_ptr_cmp(const void *a, const void *b)
{
	return thread_sp_direction > 0 ? ptr_cmp(a, b) : ptr_cmp(b, a);
}

/**
 * Compute the stack offset, for a pointer that is "above" the stack base.
 */
static inline size_t
thread_stack_ptr_offset(const void *base, const void *sp)
{
	return thread_sp_direction > 0 ? ptr_diff(sp, base) : ptr_diff(base, sp);
}

/**
 * Small thread ID.
 *
 * We count threads as they are seen, starting with 0.
 *
 * Given that, as of 2012-04-14, we are mostly mono-threaded or do not create
 * many threads dynamically, there is no need to manage the IDs in a reusable
 * way.  A simple incrementing counter will do.
 */
static unsigned thread_next_stid;

static unsigned
thread_hash(const void *key)
{
	const thread_t *t = key;

	STATIC_ASSERT(sizeof(long) == sizeof(*t));

	return integer_hash(* (ulong *) t);
}

static bool
thread_equal(const void *a, const void *b)
{
	const thread_t *ta = a, *tb = b;

	return thread_eq(*ta, *tb);
}

/**
 * Initialize global configuration.
 */
static void
thread_init(void)
{
	if G_UNLIKELY(thread_inited)
		return;

	thread_pageshift = ctz(compat_pagesize());
	thread_sp_direction = alloca_stack_direction();
	thread_inited = TRUE;
}

/**
 * Initialize the thread stack shape for the thread element.
 */
static void
thread_stack_init_shape(struct thread_element *te, const void *sp)
{
	te->stack_base = vmm_page_start(sp);

	if (thread_sp_direction < 0) {
		te->stack_base = const_ptr_add_offset(te->stack_base,
			compat_pagesize() - sizeof(void *));
	}
}

/**
 * Initialize the lock stack for the thread element.
 */
static void
thread_lock_stack_init(struct thread_element *te)
{
	struct thread_lock_stack *tls = &te->locks;

	tls->arena = omalloc(THREAD_LOCK_MAX * sizeof tls->arena[0]);
	tls->capacity = THREAD_LOCK_MAX;
	tls->count = 0;
}

/**
 * Fast computation of the Quasi Thread ID (QID) of a thread.
 *
 * @param sp		a stack pointer belonging to the thread
 *
 * The concept of QID relies on the fact that a given stack page can only
 * belong to one thread, by definition.
 */
static inline ALWAYS_INLINE thread_qid_t
thread_quasi_id_fast(const void *sp)
{
	if (sizeof(thread_qid_t) <= sizeof(unsigned)) {
		return pointer_to_ulong(sp) >> thread_pageshift;
	} else {
		uint64 qid = pointer_to_ulong(sp) >> thread_pageshift;
		return (qid >> 32) ^ (unsigned) qid;
	}
}

/**
 * Computes the Quasi Thread ID (QID) for current thread.
 */
thread_qid_t
thread_quasi_id(void)
{
	int sp;

	if G_UNLIKELY(!thread_inited)
		thread_init();

	return thread_quasi_id_fast(&sp);
}

/**
 * @return whether thread element is matching the QID.
 */
static inline ALWAYS_INLINE bool
thread_element_matches(struct thread_element *te, const thread_qid_t qid)
{
	if (NULL == te)
		return FALSE;
	if (te->last_qid == qid)
		return TRUE;

	/*
	 * This only holds for 32-bit machines:
	 *
	 * The QID is the stack page number of the thread.  The kernel needs to
	 * add at least one unmapped page between threads to detect stack overflows.
	 * Therefore, if the QID is the "upper" neighbour of the last QID, it means
	 * the stack has slightly grown since last time.  Likewise, if the QID is
	 * the "lower" neighbour of the last QID, it means the stack has slightly
	 * shrank, but it has to be the same thread.
	 */

	if (sizeof(thread_qid_t) <= sizeof(unsigned)) {
		if (te->last_qid == qid + 1 || te->last_qid == qid - 1) {
			if G_UNLIKELY(thread_stack_ptr_cmp(&qid, te->stack_base) < 0)
				thread_stack_init_shape(te, &qid);
			return TRUE;
		}
	}

	return FALSE;
}

/**
 * Get the main hash table.
 *
 * This hash table is indexed by thread_t and holds a thread element which
 * is therefore thread-private and can be used to store thread-private
 * information.
 */
static hash_table_t *
thread_get_global_hash(void)
{
	static hash_table_t *ht;

	if G_UNLIKELY(NULL == ht) {
		spinlock(&thread_private_slk);
		if (NULL == ht) {
			ht = hash_table_once_new_full_real(thread_hash, thread_equal);
			hash_table_thread_safe(ht);
		}
		spinunlock(&thread_private_slk);
	}

	return ht;
}

/**
 * Allocate a new thread element.
 */
static struct thread_element *
thread_new_element(thread_t t)
{
	struct thread_element *te;
	unsigned stid;

	g_assert(spinlock_is_held(&thread_insert_slk));

	stid = thread_next_stid;

	if G_UNLIKELY(stid >= THREAD_MAX) {
		s_error("discovered thread #%u but can only track %d threads",
			stid, THREAD_MAX);
	}

	/*
	 * Recording the current thread in the tstid[] array allows us to be able
	 * to return the new thread small ID from thread_small_id() before the
	 * allocation of the thread element is completed.
	 */

	tstid[stid] = t;

	te = omalloc0(sizeof *te);				/* Never freed! */
	te->tid = t;
	te->last_qid = (thread_qid_t) -1;
	te->pht = hash_table_once_new_real();	/* Never freed! */
	te->stid = stid;

	thread_stack_init_shape(te, &te);
	thread_lock_stack_init(te);

	threads[stid] = te;
	thread_next_stid++;		/* Created and initialized, make it visible */

	return te;
}

/**
 * Called when thread has been suspended for too long.
 */
static void
thread_timeout(const volatile struct thread_element *te)
{
	static spinlock_t thread_timeout_slk = SPINLOCK_INIT;
	unsigned i;
	unsigned ostid = (unsigned) -1;
	bool multiple = FALSE;
	struct thread_element *wte;

	spinlock(&thread_timeout_slk);

	for (i = 0; i < G_N_ELEMENTS(threads); i++) {
		const struct thread_element *xte = threads[i];

		if (0 == xte->suspend) {
			if ((unsigned) -1 == ostid)
				ostid = xte->stid;
			else {
				multiple = TRUE;
				break;		/* Concurrency update detected */
			}
		}
	}

	wte = (struct thread_element *) te;
	wte->suspend = 0;					/* Make us running again */

	spinunlock(&thread_timeout_slk);

	s_minicrit("thread #%u suspended for too long", te->stid);

	if (ostid != (unsigned) -1) {
		s_minicrit("%ssuspending thread was #%u",
			multiple ? "first " : "", ostid);
	}

	s_error("thread suspension timeout detected");
}

/**
 * Voluntarily suspend execution of the current thread, as described by the
 * supplied thread element, if it is flagged as being suspended.
 */
static void
thread_suspend_self(const volatile struct thread_element *te)
{
	time_t start = 0;
	unsigned i;

	/*
	 * We cannot let a thread holding spinlocks or mutexes to suspend itself
	 * since that could cause a deadlock with the concurrent thread that will
	 * be running.  For instance, the VMM layer could be logging a message
	 * whilst it holds an internal mutex.
	 */

	g_assert(0 == te->locks.count);

	for (i = 0; te->suspend; i++) {
		do_sched_yield();

		/*
		 * Make sure we don't stay suspended indefinitely: funnelling from
		 * other threads should occur only for a short period of time.
		 */

		if G_UNLIKELY(i != 0 && 0 == i % THREAD_SUSPEND_CHECK) {
			if (0 == start)
				start = tm_time();
			if (delta_time(tm_time_exact(), start) > THREAD_SUSPEND_TIMEOUT)
				thread_timeout(te);
		}
	}
}

/**
 * Get the thread-private element.
 *
 * If no element was already associated with the current thread, a new one
 * is created and attached to the thread.
 *
 * @return the thread-private element associated with the current thread.
 */
static struct thread_element *
thread_get_element(void)
{
	thread_qid_t qid;
	thread_t t;
	hash_table_t *ght;
	struct thread_element *te;
	unsigned idx;

	/*
	 * Look whether we already determined the thread-private element table
	 * for this thread earlier by looking in the cache, indexed by QID.
	 */

	qid = thread_quasi_id();
	idx = hashing_fold(qid, THREAD_QID_BITS);
	te = thread_qid_cache[idx];

	G_PREFETCH_R(&te->stack_base);

	if (thread_element_matches(te, qid))
		return te;

	/*
	 * No matching element was found in the cache, perform the slow lookup
	 * in the global hash table then.
	 *
	 * There's no need to grab the thread_insert_slk spinlock at this stage
	 * since the lookup is non-destructive: although the lookup will call
	 * thread_current() again during the mutex grabbing, we will either get
	 * the same QID, in which case it will be flagged busy so thread_current()
	 * will return thread_self(), or the different QID will cause a recursion
	 * here and we may use the above fast-path successfully, or fall back here.
	 *
	 * Recursion will stop at some point since the stack will not grow by one
	 * full page in these call chains, necessarily causing the same QID to be
	 * reused.  When unwinding the recursion, the item for thread_self() will
	 * be seen in the table so we won't re-create a thread element for the
	 * current thread.
	 */

	t = thread_self();
	ght = thread_get_global_hash();
	te = hash_table_lookup(ght, &t);

	/*
	 * There's no need to lock the hash table as this call can be made only
	 * once at a time per thread (the global hash table is already protected
	 * against concurrent accesses).
	 */

	if G_UNLIKELY(NULL == te) {
		/*
		 * It is the first time we're seeing this thread, record a new
		 * element in the global hash table.
		 *
		 * The reason we're surrounding hash_table_insert() with spinlocks
		 * is that the global hash table is synchronized and will grab a
		 * mutex before inserting, which will again call thread_current().
		 * In case the QID then would be different, we could come back here
		 * and create a second thread element for the same thread!
		 *
		 * The thread_current() routine checks whether the spinlock is held
		 * before deciding to call us to create a new element, thereby
		 * protecting against this race condition against ourselves, due to
		 * the fact that QIDs are not unique within a thread.
		 */

		spinlock(&thread_insert_slk);

		te = hash_table_lookup(ght, &t);
		if (NULL == te) {
			te = thread_new_element(t);
			hash_table_insert(ght, &te->tid, te);
		}

		spinunlock(&thread_insert_slk);
	}

	/*
	 * Cache result to speed-up things next time if we come back for the
	 * same thread with the same QID.
	 *
	 * We assume the value will be atomically written in memory.
	 */

	thread_qid_cache[idx] = te;
	te->last_qid = qid;

	/*
	 * Maintain lowest stack address for thread.
	 */

	if G_UNLIKELY(thread_stack_ptr_cmp(&qid, te->stack_base) < 0)
		thread_stack_init_shape(te, &qid);

	return te;
}

/**
 * Get the thread-private hash table storing the per-thread keys.
 */
static hash_table_t *
thread_get_private_hash(void)
{
	return thread_get_element()->pht;
}

/**
 * Find existing thread based on the supplied stack pointer.
 *
 * @param sp		a pointer to the stack
 *
 * @return the likely thread element to which the stack pointer could relate,
 * NULL if we cannot determine the thread.
 */
static struct thread_element *
thread_find(const void *sp)
{
	size_t i;
	struct thread_element *te = NULL;
	size_t smallest = (size_t) -1;
	thread_qid_t qid;
	unsigned idx;

	/*
	 * Since we have a stack pointer belonging to the thread we're looking,
	 * check whether we have it cached by its QID.
	 */

	qid = thread_quasi_id_fast(sp);
	idx = hashing_fold(qid, THREAD_QID_BITS);
	te = thread_qid_cache[idx];

	if (thread_element_matches(te, qid))
		return te;

	/*
	 * Perform linear lookup, looking for the thread for which the stack
	 * pointer is "above" the parameter and for which the distance to the
	 * base of the stack is the smallest.
	 */

	for (i = 0; i < thread_next_stid; i++) {
		struct thread_element *xte = threads[i];

		if (thread_stack_ptr_cmp(sp, xte->stack_base) > 0) {
			size_t offset;

			/*
			 * Pointer is "above" the stack base, track the thread whith
			 * the smallest offset relative to the stack base.
			 */

			offset = thread_stack_ptr_offset(xte->stack_base, sp);
			if (offset < smallest) {
				te = xte;
				smallest = offset;
			}
		}
	}

	/*
	 * Cache result.
	 */

	if G_LIKELY(te != NULL) {
		thread_qid_cache[idx] = te;
		te->last_qid = qid;
	}

	return te;
}

/**
 * Get thread small ID.
 */
unsigned
thread_small_id(void)
{
	struct thread_element *te;
	unsigned retries = 0, i;
	thread_t t;

retry:

	/*
	 * This call is used by logging routines, so we must be very careful
	 * about not deadlocking ourselves, yet we must use this opportunity
	 * to register the current calling thread if not already done, so try
	 * to call thread_get_element() when it is safe.
	 */

	if G_UNLIKELY(spinlock_is_held(&thread_private_slk))
		return 0;		/* Creating global hash, must be the first thread */

	if G_LIKELY(!spinlock_is_held(&thread_insert_slk))
		return thread_get_element()->stid;

	/*
	 * Locate the thread by QID or by scanning the known threads.
	 */

	te = thread_find(&te);
	if G_LIKELY(te != NULL)
		return te->stid;

	/*
	 * We may be in the thread that is being created, however we have not
	 * completed the thread installation yet.  Look in tstid[] for a
	 * matching thread.
	 */

	t = thread_self();

	for (i = 0; i < G_N_ELEMENTS(tstid); i++) {
		if G_UNLIKELY(i > thread_next_stid)
			break;
		if G_UNLIKELY(t == tstid[i])
			return i;
	}

	/*
	 * The current thread is not registered, the insertion lock is busy which
	 * means we are in the process of adding a new one but we cannot determine
	 * the exact thread ID: several threads could be requesting a small ID at
	 * the same time.
	 *
	 * We are certainly multi-threaded at this point, so wait a little bit to
	 * let other threads release the locks and retry.  After 20 loops, we
	 * abandon all hopes and abort the execution.
	 */

	if (retries++ < 20) {
		/* Thread is unknown, we should not be holding any locks */
		compat_sleep_ms(50);
		goto retry;
	}

	s_error("cannot compute thread small ID");
}

/**
 * Translate a thread ID into a small thread ID.
 *
 * @return small thread ID if thread is known, -1 otherwise.
 */
int
thread_stid_from_thread(const thread_t t)
{
	unsigned i;

	for (i = 0; i < G_N_ELEMENTS(tstid); i++) {
		if G_UNLIKELY(i > thread_next_stid)
			break;
		if G_UNLIKELY(t == tstid[i])
			return i;
	}

	return -1;
}

/**
 * Check whether thread is suspended and can be suspended right now.
 */
void
thread_check_suspended(void)
{
	struct thread_element *te;

	/*
	 * It is not critical to be in a thread that has not been seen yet, and
	 * we don't want this call to be too expensive, so detect mono-threaded
	 * conditions using a fast-path shortcut that should be correct 99.9% of
	 * the time.
	 */

	if (thread_next_stid <= 1)
		return;		/* Mono-threaded, most likely */

	te = thread_get_element();

	if (0 == te->locks.count)
		thread_suspend_self(te);
}

/**
 * Suspend other threads (advisory, not kernel-enforced).
 *
 * This is voluntary suspension, which will only occur when threads actively
 * check for supension by calling thread_check_suspended().
 *
 * It is possible to call this routine multiple times, provided each call is
 * matched with a corresponding thread_unsuspend_others().
 *
 * @return the amount of threads suspended.
 */
size_t
thread_suspend_others(void)
{
	struct thread_element *te;
	size_t i, n = 0;

	/*
	 * Must use thread_find() and not thread_get_element() to avoid taking
	 * any internal locks which could be already held from earlier (deadlock
	 * assurred) or by other threads (dealock threat if we end up needing
	 * these locks).
	 */

	te = thread_find(&te);		/* Ourselves */
	if (NULL == te)
		return 0;

retry:
	mutex_get(&thread_suspend_mtx);

	/*
	 * If we were concurrently asked to suspend ourselves, wait.
	 */

	if (te->suspend) {
		mutex_release(&thread_suspend_mtx);
		thread_suspend_self(te);
		goto retry;
	}

	for (i = 0; i < thread_next_stid; i++) {
		struct thread_element *xte = threads[i];

		if G_UNLIKELY(xte == te)
			continue;

		xte->suspend++;
		n++;
	}

	/*
	 * Make sure that we remain the sole thread running.
	 */

	te->suspend = 0;
	mutex_release(&thread_suspend_mtx);

	return n;
}

/**
 * Un-suspend all threads.
 *
 * This should only be called by a thread after it used thread_suspend_others()
 * to resume concurrent execution.
 *
 * @attention
 * If thread_suspend_others() was called multiple times, then this routine
 * must be called an identical amount of times before other threads can resume
 * their execution.  This means each call to the former must be paired with
 * a call to the latter, usually surrounding a critical section that should be
 * executed by one single thread at a time.
 *
 * @return the amount of threads unsuspended.
 */
size_t
thread_unsuspend_others(void)
{
	bool locked;
	size_t i, n = 0;
	struct thread_element *te;

	te = thread_find(&te);		/* Ourselves */
	if (NULL == te)
		return 0;

	locked = mutex_get_try(&thread_suspend_mtx);

	g_assert(locked);		/* All other threads should be sleeping */

	for (i = 0; i < thread_next_stid; i++) {
		struct thread_element *xte = threads[i];

		if G_LIKELY(xte->suspend) {
			xte->suspend--;
			n++;
		}
	}

	mutex_release(&thread_suspend_mtx);

	return n;
}

/**
 * Get current thread.
 *
 * This allows us to count the running threads as long as each thread uses
 * mutexes at some point or calls thread_current().
 */
thread_t
thread_current(void)
{
	thread_qid_t qid;
	unsigned idx;
	struct thread_element *te;

	/*
	 * We must be careful because thread_current() is what is used by mutexes
	 * to record the current thread, so we can't blindly rely on
	 * thread_get_element(), which will cause a lookup on a synchronized hash
	 * table -- that would deadly recurse.
	 *
	 * We first begin like thread_get_element() would by using the QID to fetch
	 * the current thread record: this is our fast path that is most likely
	 * to succeed and should be faster than pthread_self().
	 */

	qid = thread_quasi_id_fast(&qid);
	idx = hashing_fold(qid, THREAD_QID_BITS);
	te = thread_qid_cache[idx];

	G_PREFETCH_R(&te->stack_base);

	if (thread_element_matches(te, qid))
		return te->tid;

	/*
	 * There is no current thread record.  If this QID is marked busy, or if
	 * someone is currently creating the global hash table, then immediately
	 * return the current thread.
	 *
	 * Special care must be taken when the VMM layer is not fully inited yet,
	 * since it uses mutexes and therefore will call thread_current() as well.
	 */

	if (
		thread_qid_busy[idx] ||
		spinlock_is_held(&thread_private_slk) ||
		spinlock_is_held(&thread_insert_slk) ||
		!vmm_is_inited()
	)
		return thread_self();

	/*
	 * Mark the QID busy so that we use a short path on further recursions
	 * until we can establish a thread element.
	 *
	 * This is the part allowing us to count the running threads, since the
	 * creation of a thread element will account for the thread.
	 */

	thread_qid_busy[idx] = TRUE;

	/*
	 * Calling thread_get_element() will redo part of the work we've been
	 * doing but will also allocate and insert in the cache a new thread
	 * element for the current thread, if needed.
	 */

	te = thread_get_element();

	/*
	 * We re-cache the thread element for this QID, which may be different
	 * from the one used by thread_get_element() since it is based on the
	 * current stack pointer, and we may be near a page boundary.
	 */

	thread_qid_cache[idx] = te;
	te->last_qid = qid;
	thread_qid_busy[idx] = FALSE;

	return te->tid;
}

/**
 * Return amount of running threads.
 */
unsigned
thread_count(void)
{
	/*
	 * Relies on the fact that all running threads will, at some point, use
	 * malloc() or another call requiring a spinlock, hence calling this
	 * layer.
	 */

	return thread_next_stid;
}

/**
 * Determine whether we're a mono-threaded application.
 */
bool
thread_is_single(void)
{
	if (thread_next_stid > 1)
		return FALSE;

	(void) thread_current();		/* Counts threads */

	return 1 >= thread_next_stid;
}

/**
 * Is pointer a valid stack pointer?
 *
 * When top is NULL, we must be querying for the current thread or the routine
 * will likely return FALSE unless the pointer is in the same page as the
 * stack bottom.
 *
 * @param p		pointer to check
 * @param top	pointer to stack's top
 * @param stid	if non-NULL, filled with the small ID of the thread
 *
 * @return whether the pointer is within the bottom and the top of the stack.
 */
bool
thread_is_stack_pointer(const void *p, const void *top, unsigned *stid)
{
	struct thread_element *te;

	if G_UNLIKELY(NULL == p)
		return FALSE;

	te = thread_find(p);
	if (NULL == te)
		return FALSE;

	if (NULL == top) {
		if (thread_get_element() != te)
			return FALSE;		/* Not in the current thread */
		top = &te;
	}

	if (stid != NULL)
		*stid = te->stid;

	if (thread_sp_direction < 0) {
		/* Stack growing down, stack_base is its highest address */
		g_assert(ptr_cmp(te->stack_base, top) > 0);
		return ptr_cmp(p, top) >= 0 && ptr_cmp(p, te->stack_base) < 0;
	} else {
		/* Stack growing up, stack_base is its lowest address */
		g_assert(ptr_cmp(te->stack_base, top) < 0);
		return ptr_cmp(p, top) <= 0 && ptr_cmp(p, te->stack_base) >= 0;
	}
}

/**
 * Get thread-private data indexed by key.
 */
void *
thread_private_get(const void *key)
{
	hash_table_t *pht;
	struct thread_pvalue *pv;

	pht = thread_get_private_hash();
	pv = hash_table_lookup(pht, key);

	return NULL == pv ? NULL : pv->value;
}

/**
 * Remove thread-private data indexed by key.
 *
 * If any free-routine was registered for the value, it is invoked before
 * returning.
 *
 * @return TRUE if key existed.
 */
bool
thread_private_remove(const void *key)
{
	hash_table_t *pht;
	void *v;

	pht = thread_get_private_hash();
	if (hash_table_lookup_extended(pht, key, NULL, &v)) {
		struct thread_pvalue *pv = v;

		hash_table_remove(pht, key);
		if (pv->p_free != NULL)
			(*pv->p_free)(pv->value, pv->p_arg);
		zfree(pvzone, pv);

		return TRUE;
	} else {
		return FALSE;
	}
}

/**
 * Add thread-private data with a free routine.
 *
 * @param key		the key for the private data
 * @param value		private value to store
 * @param p_free	free-routine to invoke when key is removed
 * @param p_arg		additional opaque argument for the freeing callback
 */
void
thread_private_add_extended(const void *key, const void *value,
	thread_pvalue_free_t p_free, void *p_arg)
{
	hash_table_t *pht;
	struct thread_pvalue *pv;
	bool ok;

	if G_UNLIKELY(NULL == pvzone) {
		static spinlock_t pvzone_slk = SPINLOCK_INIT;
		spinlock(&pvzone_slk);
		if (NULL == pvzone)
			pvzone = zcreate(sizeof *pv, 0, TRUE);	/* Embedded zone */
		spinunlock(&pvzone_slk);
	}

	pv = zalloc(pvzone);
	ZERO(pv);
	pv->value = deconstify_pointer(value);
	pv->p_free = p_free;
	pv->p_arg = p_arg;

	pht = thread_get_private_hash();
	ok = hash_table_insert(pht, key, pv);

	g_assert(ok);		/* No duplicate insertions */
}

/**
 * Add thread-private data indexed by key.
 */
void
thread_private_add(const void *key, const void *value)
{
	thread_private_add_extended(key, value, NULL, NULL);
}

/**
 * Stringify the thread ID.
 *
 * @return pointer to static string
 */
const char *
thread_to_string(const thread_t t)
{
	static char buf[ULONG_DEC_BUFLEN];

	ulong_to_string_buf(t, buf, sizeof buf);
	return buf;
}

/**
 * Account or clear pending message to be emitted by some thread before
 * final exit.
 */
void
thread_pending_add(int increment)
{
	struct thread_element *te;

	te = thread_find(&te);
	if G_UNLIKELY(NULL == te)
		return;

	if (increment > 0) {
		te->pending += increment;
	} else {
		/* We may not always account when thread_find() returns NULL */
		if G_LIKELY(te->pending >= -increment)
			te->pending += increment;
		else
			te->pending = 0;
	}
}

/**
 * Report amount of pending messages registered by threads.
 *
 * This is not taking locks, so it may be slightly off.
 *
 * @return amount of pending messages.
 */
size_t
thread_pending_count(void)
{
	unsigned i;
	size_t count = 0;

	for (i = 0; i < thread_next_stid; i++) {
		count += threads[i]->pending;
	}

	return count;
}

/**
 * @return English description for lock kind.
 */
static const char *
thread_lock_kind_to_string(const enum thread_lock_kind kind)
{
	switch (kind) {
	case THREAD_LOCK_SPINLOCK:	return "spinlock";
	case THREAD_LOCK_MUTEX:		return "mutex";
	}

	return "UNKNOWN";
}

/*
 * Dump list of locks held by thread.
 */
static void
thread_lock_dump(const struct thread_element *te)
{
	const struct thread_lock_stack *tls = &te->locks;
	unsigned i;

	s_miniinfo("list of locks owned by thread #%u, most recent first:",
		te->stid);

	for (i = tls->count; i != 0; i--) {
		const struct thread_lock *l = &tls->arena[i - 1];
		const char *type;
		char buf[POINTER_BUFLEN + 2];
		char line[UINT_DEC_BUFLEN];
		const char *lnum;
		DECLARE_STR(16);

		type = thread_lock_kind_to_string(l->kind);
		buf[0] = '0';
		buf[1] = 'x';
		pointer_to_string_buf(l->lock, &buf[2], sizeof buf - 2);

		print_str("\t");		/* 0 */
		print_str(buf);			/* 1 */
		print_str(" ");			/* 2 */
		print_str(type);		/* 3 */
#ifdef SPINLOCK_DEBUG
		print_str(" from ");	/* 4 */
		switch (l->kind) {
		case THREAD_LOCK_SPINLOCK:
			{
				const spinlock_t *s = l->lock;
				lnum = print_number(line, sizeof line, s->line);
				print_str(s->file);			/* 5 */
				print_str(":");				/* 6 */
				print_str(lnum);			/* 7 */
				if (SPINLOCK_MAGIC != s->magic)
					print_str(" BAD_MAGIC");	/* 8 */
				if (s->lock != 1)
					print_str(" BAD_LOCK");		/* 9 */
			}
			break;
		case THREAD_LOCK_MUTEX:
			{
				const mutex_t *m = l->lock;
				const spinlock_t *s = &m->lock;
				lnum = print_number(line, sizeof line, s->line);
				print_str(s->file);			/* 5 */
				print_str(":");				/* 6 */
				print_str(lnum);			/* 7 */
				if (MUTEX_MAGIC != m->magic)
					print_str(" BAD_MAGIC");	/* 8 */
				if (s->lock != 1)
					print_str(" BAD_LOCK");		/* 9 */
				if (m->owner != te->tid)
					print_str(" BAD_TID");		/* 10 */
			}
			break;
		}
#endif
		if (THREAD_LOCK_MUTEX == l->kind) {
			char depth[ULONG_DEC_BUFLEN];
			const char *dnum;
			const mutex_t *m = l->lock;

			dnum = print_number(depth, sizeof depth, m->depth);
			print_str(" (depth=");		/* 11 */
			print_str(dnum);			/* 12 */
			print_str(")");				/* 13 */
			if (0 == m->depth)
				print_str(" BAD_DEPTH");	/* 14 */
		}
		print_str("\n");		/* 15 */
		flush_err_str();
	}
}

/**
 * Dump locks held by current thread, most recently taken first.
 */
void
thread_lock_current_dump(void)
{
	struct thread_element *te;
	
	te = thread_find(&te);
	if G_UNLIKELY(NULL == te)
		return;

	thread_lock_dump(te);
}

/**
 * Account for spinlock / mutex acquisition by current thread.
 */
void
thread_lock_got(const void *lock, enum thread_lock_kind kind)
{
	struct thread_element *te;
	struct thread_lock_stack *tls;
	struct thread_lock *l;

	/*
	 * Don't use thread_get_element(), we MUST not be taking any locks here
	 * since we're in a lock path.  We could end-up re-locking the lock we're
	 * accounting for.  Also we don't want to create a new thread if the
	 * thread element is already in the process of being created.
	 */

	te = thread_find(&te);
	if G_UNLIKELY(NULL == te)
		return;

	tls = &te->locks;

	if G_UNLIKELY(tls->capacity == tls->count) {
		if (tls->overflow)
			return;				/* Already signaled, we're crashing */
		tls->overflow = TRUE;
		s_minicrit("thread #%u overflowing its lock stack", te->stid);
		thread_lock_dump(te);
		s_error("too many locks grabbed simultaneously");
	}

	l = &tls->arena[tls->count++];
	l->lock = lock;
	l->kind = kind;
}

/**
 * Account for spinlock / mutex release by current thread.
 */
void
thread_lock_released(const void *lock, enum thread_lock_kind kind)
{
	struct thread_element *te;
	struct thread_lock_stack *tls;
	const struct thread_lock *l;
	unsigned i;

	/*
	 * For the same reasons as in thread_lock_add(), lazily grab the thread
	 * element.  Note that we may be in a situation where we did not get a
	 * thread element at lock time but are able to get one now.
	 */

	te = thread_find(&te);
	if G_UNLIKELY(NULL == te)
		return;

	tls = &te->locks;

	if G_UNLIKELY(0 == tls->count)
		return;

	/*
	 * If lock is the top of the stack, we're done.
	 */

	l = &tls->arena[tls->count - 1];

	if G_LIKELY(l->lock == lock) {
		g_assert(l->kind == kind);

		tls->count--;

		/*
		 * If the thread no longer holds any locks and it has to be suspended,
		 * now is a good (and safe) time to do it.
		 */

		if G_UNLIKELY(te->suspend && 0 == tls->count)
			thread_suspend_self(te);

		return;
	}

	/*
	 * Since the lock was not the one at the top of the stack, then it must be
	 * absent in the whole stack, or we have an out-of-order lock release.
	 */

	if (tls->overflow)
		return;				/* Stack overflowed, we're crashing */

	for (i = 0; i < tls->count; i++) {
		const struct thread_lock *ol = &tls->arena[i];

		if (ol->lock == lock) {
			tls->overflow = TRUE;	/* Avoid any overflow problems now */
			s_minicrit("thread #%u releases %s %p at inner position %u/%u",
				te->stid, thread_lock_kind_to_string(kind), lock, i + 1,
				tls->count);
			thread_lock_dump(te);
			s_error("out-of-order %s release",
				thread_lock_kind_to_string(kind));
		}
	}
}

/**
 * Check whether current thread already holds a lock.
 *
 * @param lock		the address of a mutex of a spinlock
 *
 * @return TRUE if lock was registered in the current thread.
 */
bool
thread_lock_holds(const volatile void *lock)
{
	struct thread_element *te;
	struct thread_lock_stack *tls;
	unsigned i;

	/*
	 * For the same reasons as in thread_lock_add(), lazily grab the thread
	 * element.  Note that we may be in a situation where we did not get a
	 * thread element at lock time but are able to get one now.
	 */

	te = thread_find(&te);
	if G_UNLIKELY(NULL == te)
		return FALSE;

	tls = &te->locks;

	if G_UNLIKELY(0 == tls->count)
		return FALSE;

	for (i = 0; i < tls->count; i++) {
		const struct thread_lock *l = &tls->arena[i];

		if (l->lock == lock)
			return TRUE;
	}

	return FALSE;
}

/**
 * @return amount of locks held by the current thread.
 */
size_t
thread_lock_count(void)
{
	struct thread_element *te;

	te = thread_find(&te);
	if G_UNLIKELY(NULL == te)
		return 0;

	return te->locks.count;
}

/**
 * Find who owns a lock, and what kind of lock it is.
 *
 * @param lock		the lock address
 * @param kind		where type of lock is written, if owner found
 *
 * @return thread owning a lock, NULL if we can't find it.
 */
static struct thread_element *
thread_lock_owner(const volatile void *lock, enum thread_lock_kind *kind)
{
	unsigned i;

	/*
	 * We don't stop other threads because we're called in a deadlock
	 * situation so it's highly unlikely that the thread owning the lock
	 * will suddenly choose to release it.
	 */

	for (i = 0; i < thread_next_stid; i++) {
		struct thread_element *te = threads[i];
		struct thread_lock_stack *tls = &te->locks;
		unsigned j;

		for (j = 0; j < tls->count; j++) {
			const struct thread_lock *l = &tls->arena[j];

			if (l->lock == lock) {
				*kind = l->kind;
				return te;
			}
		}
	}

	return NULL;
}

/**
 * Report a deadlock condition whilst attempting to get a lock.
 *
 * This is only executed once per thread, since a deadlock is an issue that
 * will only be resolved through process termination.
 */
void
thread_lock_deadlock(const volatile void *lock)
{
	struct thread_element *te;
	struct thread_element *towner;
	static bool deadlocked;
	enum thread_lock_kind kind;
	unsigned i;

	if (deadlocked)
		return;				/* Recursion, avoid problems */

	deadlocked = TRUE;
	atomic_mb();

	te = thread_find(&te);
	if G_UNLIKELY(NULL == te) {
		s_miniinfo("no thread to list owned locks");
		return;
	}

	if (te->deadlocked)
		return;		/* Do it once per thread since there is no way out */

	te->deadlocked = TRUE;
	towner = thread_lock_owner(lock, &kind);

	if (NULL == towner || towner == te) {
		s_minicrit("thread #%u deadlocked whilst waiting on %s%s%p, "
			"owned by %s", te->stid,
			NULL == towner ? "" : thread_lock_kind_to_string(kind),
			NULL == towner ? "" : " ",
			lock, NULL == towner ? "nobody" : "itself");
	} else {
		s_minicrit("thread #%u deadlocked whilst waiting on %s %p, "
			"owned by thread #%u", te->stid,
			thread_lock_kind_to_string(kind), lock, towner->stid);
	}

	thread_lock_dump(te);
	if (towner != NULL && towner != te)
		thread_lock_dump(towner);

	/*
	 * Mark all the threads as overflowing their lock stack.
	 *
	 * That way we'll silently ignore lock recording overflows and will
	 * become totally permissive about out-of-order releases.
	 */

	for (i = 0; i < thread_next_stid; i++) {
		struct thread_element *xte = threads[i];
		struct thread_lock_stack *tls = &xte->locks;

		atomic_mb();
		tls->overflow = TRUE;
		atomic_mb();
	}

	/*
	 * Disable all locks: spinlocks and mutexes will be granted immediately,
	 * preventing further deadlocks at the cost of a possible crash.  However,
	 * this allows us to maybe collect information that we couldn't otherwise
	 * get at, so it's worth the risk.
	 */

	spinlock_crash_mode();	/* Allow all mutexes and spinlocks to be grabbed */
	mutex_crash_mode();		/* Allow release of all mutexes */

	s_miniinfo("attempting to unwind current stack:");
	stacktrace_where_safe_print_offset(STDERR_FILENO, 1);
}

/* vi: set ts=4 sw=4 cindent: */
