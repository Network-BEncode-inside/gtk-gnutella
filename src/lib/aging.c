/*
 * Copyright (c) 2004, 2012 Raphael Manfredi
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
 * Hash table with aging key/value pairs, removed automatically after
 * some time has elapsed.
 *
 * All the entries in the table are given the same lifetime, with a granularity
 * of one second.  It is however possible to revitalize an entry being looked-up
 * by restoring its initial lifetime (as if it had been removed and reinserted).
 *
 * @author Raphael Manfredi
 * @date 2004, 2012
 */

#include "common.h"

#include "aging.h"
#include "cq.h"
#include "elist.h"
#include "hashing.h"
#include "hikset.h"
#include "misc.h"
#include "mutex.h"
#include "tm.h"
#include "walloc.h"

#include "override.h"		/* Must be the last header included */

#define AGING_CALLOUT	1500	/**< Cleanup every 1.5 seconds */

enum aging_magic {
	AGING_MAGIC	= 0x38e2fac3
};

/**
 * The hash table is the central piece, but we also have a freeing callbacks,
 * since the entries expire automatically after some time has elapsed.
 */
struct aging {
	enum aging_magic magic;		/**< Magic number */
	int delay;					/**< Initial aging delay, in seconds */
	hikset_t *table;			/**< The table holding values */
	cperiodic_t *gc_ev;			/**< Periodic garbage collecting event */
	mutex_t *lock;				/**< Optional thread-safe lock */
	free_keyval_fn_t kvfree;	/**< Freeing callback for key/value pairs */
	elist_t list;				/**< List of items in table, oldest first */
};

static void
aging_check(const aging_table_t * const ag)
{
	g_assert(ag != NULL);
	g_assert(ag->magic == AGING_MAGIC);
}

/**
 * We wrap the values we insert in the table, since each value must keep
 * track of its insertion time, and cleanup event.
 *
 * Because the aging_value structure (inserted in the table) refers to the key,
 * we can use a hikset instead of a hash table, which saves a pointer for each
 * entry.
 */
struct aging_value {
	void *value;			/**< The value they inserted in the table */
	void *key;				/**< The associated key object */
	time_t last_insert;		/**< Last insertion time */
	link_t lk;				/**< Embedded link to chain items together */
};

/*
 * Thread-safe synchronization support.
 */

#define aging_synchronize(a) G_STMT_START {			\
	if G_UNLIKELY((a)->lock != NULL) { 				\
		aging_table_t *wa = deconstify_pointer(a);	\
		mutex_lock(wa->lock);						\
	}												\
} G_STMT_END

#define aging_unsynchronize(a) G_STMT_START {		\
	if G_UNLIKELY((a)->lock != NULL) { 				\
		aging_table_t *wa = deconstify_pointer(a);	\
		mutex_unlock(wa->lock);						\
	}												\
} G_STMT_END

#define aging_return(a, v) G_STMT_START {			\
	if G_UNLIKELY((a)->lock != NULL) 				\
		mutex_unlock((a)->lock);					\
	return v;										\
} G_STMT_END

#define aging_return_void(a) G_STMT_START {			\
	if G_UNLIKELY((a)->lock != NULL) 				\
		mutex_unlock((a)->lock);					\
	return;											\
} G_STMT_END

#define assert_aging_locked(a) G_STMT_START {		\
	if G_UNLIKELY((a)->lock != NULL) 				\
		assert_mutex_is_owned((a)->lock);			\
} G_STMT_END

/**
 * Free keys and values from the aging table.
 */
static void
aging_free(void *value, void *data)
{
	struct aging_value *aval = value;
	aging_table_t *ag = data;

	aging_check(ag);
	assert_aging_locked(ag);

	if (ag->kvfree != NULL)
		(*ag->kvfree)(aval->key, aval->value);

	elist_remove(&ag->list, aval);
	WFREE(aval);
}

/**
 * Periodic garbage collecting routine.
 */
static bool
aging_gc(void *obj)
{
	aging_table_t *ag = obj;
	time_t now = tm_time();
	struct aging_value *aval;

	aging_check(ag);

	aging_synchronize(ag);

	g_assert(elist_count(&ag->list) == hikset_count(ag->table));

	while (NULL != (aval = elist_head(&ag->list))) {
		if (delta_time(now, aval->last_insert) <= ag->delay)
			break;			/* List is sorted, oldest items first */
		hikset_remove(ag->table, aval->key);
		aging_free(aval, ag);
	}

	aging_return(ag, TRUE);			/* Keep calling */
}

/**
 * Create new aging container, where keys/values expire and need to be freed.
 * Values are either integers (cast to pointers) or refer to real objects.
 *
 * @param delay		the aging delay, in seconds, for entries
 * @param hash		the hashing function for the keys in the hash table
 * @param eq		the equality function for the keys in the hash table
 * @param kvfree	the key/value pair freeing callback, NULL if none.
 *
 * @return opaque handle to the container.
 */
aging_table_t *
aging_make(int delay, hash_fn_t hash, eq_fn_t eq, free_keyval_fn_t kvfree)
{
	aging_table_t *ag;

	WALLOC0(ag);
	ag->magic = AGING_MAGIC;
	ag->table = hikset_create_any(
		offsetof(struct aging_value, key),
		NULL == hash ? pointer_hash : hash, eq);
	ag->kvfree = kvfree;
	delay = MAX(delay, 1);
	delay = MIN(delay, INT_MAX / 1000);
	ag->delay = delay;
	elist_init(&ag->list, offsetof(struct aging_value, lk));
	ag->gc_ev = cq_periodic_main_add(AGING_CALLOUT, aging_gc, ag);

	aging_check(ag);
	return ag;
}

/**
 * Destroy container, freeing all keys and values, and nullify pointer.
 */
void
aging_destroy(aging_table_t **ag_ptr)
{
	aging_table_t *ag = *ag_ptr;

	if (ag) {
		aging_check(ag);

		aging_synchronize(ag);

		hikset_foreach(ag->table, aging_free, ag);
		hikset_free_null(&ag->table);
		cq_periodic_remove(&ag->gc_ev);

		if (ag->lock != NULL) {
			mutex_destroy(ag->lock);
			WFREE(ag->lock);
		}

		ag->magic = 0;
		WFREE(ag);
		*ag_ptr = NULL;
	}
}

/**
 * Mark newly created aging table as being thread-safe.
 *
 * This will make all external operations on the table thread-safe.
 */
void
aging_thread_safe(aging_table_t *ag)
{
	aging_check(ag);
	g_assert(NULL == ag->lock);

	WALLOC0(ag->lock);
	mutex_init(ag->lock);
}

/**
 * Lock the aging table to allow a sequence of operations to be atomically
 * conducted.
 *
 * It is possible to lock the table several times as long as each locking
 * is paired with a corresponding unlocking in the execution flow.
 *
 * The table must have been marked thread-safe already.
 */
void
aging_lock(aging_table_t *ag)
{
	aging_check(ag);
	g_assert_log(ag->lock != NULL,
		"%s(): aging table %p not marked thread-safe", G_STRFUNC, ag);

	mutex_lock(ag->lock);
}

/*
 * Release lock on aging table.
 *
 * The table must have been marked thread-safe already and locked by the
 * calling thread.
 */
void
aging_unlock(aging_table_t *ag)
{
	aging_check(ag);
	g_assert_log(ag->lock != NULL,
		"%s(): aging table %p not marked thread-safe", G_STRFUNC, ag);

	mutex_unlock(ag->lock);
}

/**
 * Lookup value in table.
 */
void *
aging_lookup(const aging_table_t *ag, const void *key)
{
	struct aging_value *aval;
	void *data;

	aging_check(ag);

	aging_synchronize(ag);

	aval = hikset_lookup(ag->table, key);
	data = aval == NULL ? NULL : aval->value;

	aging_return(ag, data);
}

/**
 * Return entry age in seconds, (time_delta_t) -1  if not found.
 */
time_delta_t
aging_age(const aging_table_t *ag, const void *key)
{
	struct aging_value *aval;
	time_delta_t age;

	aging_check(ag);

	aging_synchronize(ag);

	aval = hikset_lookup(ag->table, key);
	age = aval == NULL ?
		(time_delta_t) -1 : delta_time(tm_time(), aval->last_insert);

	aging_return(ag, age);
}

/**
 * Lookup value in table, and if found, revitalize entry, restoring the
 * initial lifetime the key/value pair had at insertion time.
 */
void *
aging_lookup_revitalise(aging_table_t *ag, const void *key)
{
	struct aging_value *aval;
	void *data;

	aging_check(ag);

	aging_synchronize(ag);

	aval = hikset_lookup(ag->table, key);

	if (aval != NULL) {
		aval->last_insert = tm_time();
		elist_moveto_tail(&ag->list, aval);
	}

	data = NULL == aval ? NULL : aval->value;

	aging_return(ag, data);
}

/**
 * Remove key from the table, freeing it if we have a key free routine.
 *
 * @return whether key was found and subsequently removed.
 */
bool
aging_remove(aging_table_t *ag, const void *key)
{
	struct aging_value *aval;
	void *ovalue;
	bool found;

	aging_check(ag);

	aging_synchronize(ag);

	if (!hikset_lookup_extended(ag->table, key, &ovalue)) {
		found = FALSE;
		goto done;
	}

	aval = ovalue;

	hikset_remove(ag->table, aval->key);
	aging_free(aval, ag);

	found = TRUE;

done:
	aging_return(ag, found);
}

/**
 * Add value to the table.
 *
 * If it was already present, its lifetime is reset to the aging delay.
 *
 * The key argument is freed immediately if there is a free routine for
 * keys and the key was present in the table.
 *
 * The previous value is freed and replaced by the new one if there is
 * an insertion conflict and the key pointers are different.
 */
void
aging_insert(aging_table_t *ag, const void *key, void *value)
{
	bool found;
	void *ovalue;
	time_t now = tm_time();
	struct aging_value *aval;

	aging_check(ag);

	aging_synchronize(ag);

	found = hikset_lookup_extended(ag->table, key, &ovalue);
	if (found) {
		aval = ovalue;

		if (aval->key != key && ag->kvfree != NULL) {
			/*
			 * We discard the new and keep the old key instead.
			 * That way, we don't have to update the hash table.
			 */

			(*ag->kvfree)(deconstify_pointer(key), aval->value);
		}

		/*
		 * Value existed for this key, reset its lifetime by moving the
		 * entry to the tail of the list.
		 */

		aval->value = value;
		aval->last_insert = now;
		elist_moveto_tail(&ag->list, aval);
	} else {
		WALLOC(aval);
		aval->value = value;
		aval->key = deconstify_pointer(key);
		aval->last_insert = now;
		hikset_insert(ag->table, aval);
		elist_append(&ag->list, aval);
	}

	aging_return_void(ag);
}

/**
 * @return amount of entries held in aging table.
 */
size_t
aging_count(const aging_table_t *ag)
{
	size_t count;

	aging_check(ag);

	aging_synchronize(ag);
	count = hikset_count(ag->table);
	aging_return(ag, count);
}

/* vi: set ts=4: sw=4 cindent: */
