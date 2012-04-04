/*
 * Copyright (c) 2003, Christian Biere
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
 * List with fast indexing of items.
 *
 * An hashlist is a dual structure where data are both stored in a two-way
 * list, preserving ordering, and indexed in a hash table.
 *
 * This structure can quickly determine whether it contains some piece of
 * data, as well as quickly remove data.  It can be iterated over, in the
 * order of the items or in reverse order.
 *
 * It is NOT a hash table preserving the order of keys.  This structure only
 * stores items, not an association between a key and a value.
 *
 * @author Christian Biere
 * @date 2003
 */

#include "common.h"

#include "hashlist.h"
#include "elist.h"
#include "hashing.h"
#include "htable.h"
#include "misc.h"
#include "unsigned.h"
#include "walloc.h"

#include "override.h"		/* Must be the last header included */

enum hash_list_magic { HASH_LIST_MAGIC = 0x338954fdU };

struct hash_list {
	enum hash_list_magic magic;
	unsigned stamp;
	htable_t *ht;
	elist_t list;
	int refcount;
};

struct hash_list_item {
	const void *key;	/* The "key" is the data we store in the hashlist */
	link_t lnk;			/* The embedded link to chain items together */
};

#define ITEM(l)	elist_item((l), struct hash_list_item, lnk);

enum hash_list_iter_magic { HASH_LIST_ITER_MAGIC = 0x438954efU };

enum hash_list_iter_direction {
	HASH_LIST_ITER_UNDEFINED,
	HASH_LIST_ITER_FORWARDS,
	HASH_LIST_ITER_BACKWARDS
};

struct hash_list_iter {
	enum hash_list_iter_magic magic;
	enum hash_list_iter_direction dir;
	unsigned stamp;
	hash_list_t *hl;
	link_t *prev, *next;
	struct hash_list_item *item;
};

static G_GNUC_HOT void
hash_list_iter_check(const hash_list_iter_t * const iter)
{
	g_assert(NULL != iter);
	g_assert(HASH_LIST_ITER_MAGIC == iter->magic);
	g_assert(NULL != iter->hl);
	g_assert(iter->hl->refcount > 0);
	g_assert(iter->hl->stamp == iter->stamp);
}

#if 0
#define USE_HASH_LIST_REGRESSION
#endif

#define equiv(p,q)	(!(p) == !(q))

#ifdef USE_HASH_LIST_REGRESSION
static inline void
hash_list_regression(const hash_list_t * const hl)
{
	g_assert(NULL != hl->ht);
	g_assert(elist_count(&hl->list) == htable_count(hl->ht));
	g_assert(elist_count(&hl->list) == elist_length(elist_first(&hl->list)));
}
#else
#define hash_list_regression(hl)
#endif

static G_GNUC_HOT void
hash_list_check(const hash_list_t * const hl)
{
	g_assert(NULL != hl);
	g_assert(HASH_LIST_MAGIC == hl->magic);
	g_assert(hl->refcount > 0);
	hash_list_regression(hl);
}

/*
 * With TRACK_MALLOC, the routines hash_list_new() and hash_list_free()
 * are trapped by macros, but the routines need to be defined here,
 * since they are called directly from within malloc.c.
 */
#ifdef TRACK_MALLOC
#undef hash_list_new
#undef hash_list_free
#endif

/*
 * If walloc() and wfree() are remapped to malloc routines and they enabled
 * TRACK_MALLOC as well, then hash_list_new() and hash_list_free() are
 * wrapped within malloc.c, and the recording of the allocated descriptors
 * happens there.  So we must NOT use g_malloc() and g_free() but use
 * raw malloc() and free() instead, to avoid duplicate tracking.
 */
#if defined(REMAP_ZALLOC) && defined(TRACK_MALLOC)
#undef walloc
#undef wfree
#undef malloc
#undef free
#define walloc(s)	malloc(s)
#define wfree(p,s)	free(p)
#endif

/**
 * Create a new hash list.
 */
hash_list_t *
hash_list_new(hash_fn_t hash_func, eq_fn_t eq_func)
{
	hash_list_t *hl;

	WALLOC(hl);
	hl->ht = htable_create_any(
		NULL == hash_func ? pointer_hash : hash_func, NULL, eq_func);
	elist_init(&hl->list, offsetof(struct hash_list_item, lnk));
	hl->refcount = 1;
	hl->stamp = (unsigned) HASH_LIST_MAGIC + 1;
	hl->magic = HASH_LIST_MAGIC;
	hash_list_regression(hl);

	return hl;
}

/**
 * Dispose of the data structure, but not of the items it holds.
 *
 * @param hl_ptr	pointer to the variable containing the address of the list
 *
 * As a side effect, the variable containing the address of the list
 * is nullified, since it is no longer allowed to refer to the structure.
 */
void
hash_list_free(hash_list_t **hl_ptr)
{
	g_assert(NULL != hl_ptr);

	if (*hl_ptr) {
		hash_list_t *hl = *hl_ptr;
		link_t *lk, *next;

		hash_list_check(hl);

		if (--hl->refcount != 0) {
			g_carp("%s: hash list is still referenced! "
				"(hl=%p, hl->refcount=%d)",
				G_STRFUNC, cast_to_pointer(hl), hl->refcount);
		}

		htable_free_null(&hl->ht);

		for (lk = elist_first(&hl->list); lk != NULL; lk = next) {
			struct hash_list_item *item = ITEM(lk);
			next = elist_next(lk);	/* Embedded, get next before freeing */
			WFREE(item);
		}

		elist_discard(&hl->list);
		hl->magic = 0;
		WFREE(hl);
		*hl_ptr = NULL;
	}
}

static void
hash_list_freecb_wrapper(void *data, void *user_data)
{
	free_fn_t freecb = cast_pointer_to_func(user_data);
	struct hash_list_item *item = data;

	(*freecb)(deconstify_pointer(item->key));
	item->key = NULL;
}

/**
 * Dispose of all the items remaining in the list, applying the supplied
 * free callback on all the items, then freeing the hash_list_t container
 * and nullifying its pointer.
 */
void
hash_list_free_all(hash_list_t **hl_ptr, free_fn_t freecb)
{
	g_assert(hl_ptr != NULL);
	g_assert(freecb != NULL);

	if (*hl_ptr != NULL) {
		hash_list_t *hl = *hl_ptr;

		hash_list_check(hl);
		elist_foreach(&hl->list, hash_list_freecb_wrapper,
			cast_func_to_pointer(freecb));
		hash_list_free(hl_ptr);
	}
}

static void
hash_list_insert_item(hash_list_t *hl, struct hash_list_item *item)
{
	g_assert(!htable_contains(hl->ht, item->key));
	htable_insert(hl->ht, item->key, item);

	/*
	 * Insertion in the list is "safe" with respect to iterators.
	 *
	 * Although there is no guarantee the inserted item will be iterated
	 * over (if it lies before the iterating point), there is no invalidation
	 * of the "next" and "prev" entries that we can have pre-computed in the
	 * iterator, nor is there the risk that we would iterate forever.  All
	 * existing references to linkable cells will remain valid after the
	 * insertion took place.
	 *
	 * Therefore, do not update the hl->stamp value.
	 */

	hash_list_regression(hl);
}

/**
 * Append `key' to the list.
 *
 * It is safe to call this routine whilst iterating.
 */
void
hash_list_append(hash_list_t *hl, const void *key)
{
	struct hash_list_item *item;

	hash_list_check(hl);

	WALLOC(item);
	item->key = key;
	elist_append(&hl->list, item);
	hash_list_insert_item(hl, item);
}

/**
 * Prepend `key' to the list.
 *
 * It is safe to call this routine whilst iterating.
 */
void
hash_list_prepend(hash_list_t *hl, const void *key)
{
	struct hash_list_item *item;

	hash_list_check(hl);

	WALLOC(item);
	item->key = key;
	elist_prepend(&hl->list, item);
	hash_list_insert_item(hl, item);
}

/**
 * Insert `key' into the list.
 *
 * It is safe to call this routine whilst iterating although there is no
 * guarantee as to whether the iteration will see the new item.
 */
void
hash_list_insert_sorted(hash_list_t *hl, const void *key, cmp_fn_t func)
{
	link_t *lk;

	hash_list_check(hl);
	g_assert(NULL != func);
	g_assert(!htable_contains(hl->ht, key));

	for (lk = elist_first(&hl->list); lk != NULL; lk = elist_next(lk)) {
		struct hash_list_item *item = ITEM(lk);
		if ((*func)(key, item->key) <= 0)
			break;
	}

	if (NULL == lk) {
		hash_list_append(hl, key);
	} else {
		struct hash_list_item *item;

		WALLOC(item);
		item->key = key;

		/* Inserting ``item'' before ``lk'' */

		elist_link_insert_before(&hl->list, lk, &item->lnk);
		hash_list_insert_item(hl, item);
	}
}

static int
sort_wrapper(const void *a, const void *b, void *data)
{
	cmp_fn_t func = (cmp_fn_t) cast_pointer_to_func(data);
	const struct hash_list_item *ha = a;
	const struct hash_list_item *hb = b;

	return (*func)(ha->key, hb->key);
}

/**
 * Sort the list with ``func'' comparing keys.
 */
void
hash_list_sort(hash_list_t *hl, cmp_fn_t func)
{
	hash_list_check(hl);
	g_assert(1 == hl->refcount);
	g_assert(NULL != func);

	elist_sort_with_data(&hl->list, sort_wrapper, func_to_pointer(func));
}

struct sort_with_data {
	cmp_data_fn_t func;
	void *data;
};

static int
sort_data_wrapper(const void *a, const void *b, void *data)
{
	struct sort_with_data *ctx = data;
	const struct hash_list_item *ha = a;
	const struct hash_list_item *hb = b;

	return (*ctx->func)(ha->key, hb->key, ctx->data);
}

/**
 * Sort the list with ``func'' comparing keys, using external data in addition
 * to the keys to make the comparison.
 */
void
hash_list_sort_with_data(hash_list_t *hl, cmp_data_fn_t func, void *data)
{
	struct sort_with_data ctx;

	hash_list_check(hl);
	g_assert(1 == hl->refcount);
	g_assert(NULL != func);

	ctx.func = func;
	ctx.data = data;

	elist_sort_with_data(&hl->list, sort_data_wrapper, &ctx);
}

/**
 * Randomly shuffle the list.
 */
void
hash_list_shuffle(hash_list_t *hl)
{
	hash_list_check(hl);
	g_assert(1 == hl->refcount);

	elist_shuffle(&hl->list);
}

/**
 * Remove specified item.
 *
 * @return the original key.
 */
static void * 
hash_list_remove_item(hash_list_t *hl, struct hash_list_item *item)
{
	void *key;

	g_assert(item);

	key = deconstify_pointer(item->key);
	htable_remove(hl->ht, key);
	elist_link_remove(&hl->list, &item->lnk);
	WFREE(item);

	hl->stamp++;		/* Unsafe operation when iterating */

	hash_list_regression(hl);
	return key;
}

enum hash_list_position_magic { HASH_LIST_POSITION_MAGIC = 0x169eede3U };

struct hash_list_position {
	enum hash_list_position_magic magic;
	hash_list_t *hl;
	link_t *prev;
	unsigned stamp;
};

static inline void
hash_list_position_check(const struct hash_list_position * const hlp)
{
	g_assert(hlp != NULL);
	g_assert(HASH_LIST_POSITION_MAGIC == hlp->magic);
	hash_list_check(hlp->hl);
}

/**
 * Forget a position token.
 */
void
hash_list_forget_position(void *position)
{
	struct hash_list_position *pt = position;

	hash_list_position_check(pt);
	pt->magic = 0;
	WFREE(pt);
}

/**
 * Insert key at the saved position, obtained through a previous
 * hash_list_remove_position() call.
 *
 * The position token is destroyed.
 */
void
hash_list_insert_position(hash_list_t *hl, const void *key, void *position)
{
	struct hash_list_position *pt = position;
	struct hash_list_item *item;

	hash_list_check(hl);
	hash_list_position_check(pt);
	g_assert(1 == hl->refcount);
	g_assert(pt->hl == hl);
	g_assert(pt->stamp == hl->stamp);

	WALLOC(item);
	item->key = key;
	elist_link_insert_after(&hl->list, pt->prev, &item->lnk);
	hash_list_insert_item(hl, item);

	hash_list_forget_position(position);
}

/**
 * Remove `data' from the list but remembers the item's position so that
 * re-insertion can happen at the same place using the supplied token.
 *
 * If no re-insertion is required, the token must be freed with
 * hash_list_forget_position().
 *
 * @return a token that can be used to re-insert the key at the same
 * position in the list via hash_list_insert_position(), or NULL if
 * the data was not found.
 */
void *
hash_list_remove_position(hash_list_t *hl, const void *key)
{
	struct hash_list_item *item;
	struct hash_list_position *pt;

	hash_list_check(hl);
	g_assert(1 == hl->refcount);

	item = htable_lookup(hl->ht, key);
	if (NULL == item)
		return NULL;

	/*
	 * Record position in the list so that re-insertion can happen after
	 * the predecessor of the item.  For sanity checks, we save the hash_list_t
	 * object as well to make sure items are re-inserted in the proper list!
	 *
	 * No unsafe update (moving / deletion) must happen between the removal and
	 * the re-insertion, and this is checked by the saved stamp.
	 */

	WALLOC(pt);
	pt->magic = HASH_LIST_POSITION_MAGIC;
	pt->hl = hl;
	pt->prev = elist_prev(&item->lnk);
	pt->stamp = hl->stamp;

	hash_list_remove_item(hl, item);

	return pt;
}

/**
 * Remove `data' from the list.
 *
 * @return the data that was associated with the given key.
 */
void *
hash_list_remove(hash_list_t *hl, const void *key)
{
	struct hash_list_item *item;

	hash_list_check(hl);
	g_assert(1 == hl->refcount);

	item = htable_lookup(hl->ht, key);
	return item ? hash_list_remove_item(hl, item) : NULL;
}

/**
 * Remove head item from the list.
 *
 * @return the data that was stored there.
 */
void *
hash_list_remove_head(hash_list_t *hl)
{
	struct hash_list_item *item;

	hash_list_check(hl);

	item = elist_head(&hl->list);
	return NULL == item ? NULL : hash_list_remove_item(hl, item);
}

/**
 * Remove tail item from the list.
 *
 * @return the data that was stored there.
 */
void *
hash_list_remove_tail(hash_list_t *hl)
{
	struct hash_list_item *item;

	hash_list_check(hl);

	item = elist_tail(&hl->list);
	return NULL == item ? NULL : hash_list_remove_item(hl, item);
}

/**
 * Remove head item from the list.
 *
 * @return the data that was stored there.
 */
void *
hash_list_shift(hash_list_t *hl)
{
	struct hash_list_item *item;

	hash_list_check(hl);
	g_assert(1 == hl->refcount);

	item = elist_head(&hl->list);
	return NULL == item ? NULL : hash_list_remove_item(hl, item);
}

/**
 * Clear the list, removing all items.
 */
void
hash_list_clear(hash_list_t *hl)
{
	hash_list_check(hl);
	g_assert(1 == hl->refcount);

	while (0 != elist_count(&hl->list)) {
		struct hash_list_item *item = elist_head(&hl->list);
		hash_list_remove_item(hl, item);
	}
}

/**
 * @returns the data associated with the last item, or NULL if none.
 */
void *
hash_list_tail(const hash_list_t *hl)
{
	struct hash_list_item *item;

	hash_list_check(hl);

	item = elist_tail(&hl->list);
	return NULL == item ? NULL : deconstify_pointer(item->key);
}

/**
 * @returns the first item of the list, or NULL if none.
 */
void *
hash_list_head(const hash_list_t *hl)
{
	struct hash_list_item *item;

	hash_list_check(hl);

	item = elist_head(&hl->list);
	return NULL == item ? NULL : deconstify_pointer(item->key);
}

/**
 * Move entry to the head of the list.
 */
void
hash_list_moveto_head(hash_list_t *hl, const void *key)
{
	struct hash_list_item *item;

	hash_list_check(hl);
	g_assert(1 == hl->refcount);
	g_assert(size_is_positive(elist_count(&hl->list)));

	item = htable_lookup(hl->ht, key);
	g_assert(item != NULL);

	/*
	 * Remove item from list and insert it back at the head.
	 */

	if (elist_first(&hl->list) != &item->lnk) {
		elist_link_remove(&hl->list, &item->lnk);
		elist_link_prepend(&hl->list, &item->lnk);
	}

	hl->stamp++;

	hash_list_regression(hl);
}

/**
 * Move entry to the tail of the list.
 */
void
hash_list_moveto_tail(hash_list_t *hl, const void *key)
{
	struct hash_list_item *item;

	hash_list_check(hl);
	g_assert(1 == hl->refcount);
	g_assert(size_is_positive(elist_count(&hl->list)));

	item = htable_lookup(hl->ht, key);
	g_assert(item != NULL);

	/*
	 * Remove item from list and insert it back at the tail.
	 */

	if (elist_last(&hl->list) != &item->lnk) {
		elist_link_remove(&hl->list, &item->lnk);
		elist_link_append(&hl->list, &item->lnk);
	}

	hl->stamp++;

	hash_list_regression(hl);
}

/**
 * @returns the length of the list.
 */
unsigned
hash_list_length(const hash_list_t *hl)
{
	hash_list_check(hl);

	return elist_count(&hl->list);
}

/**
 * Extract the list of items so that the caller can iterate at will over
 * it as sort it.  The caller must dispose of that list via g_list_free().
 * The underlying data is not copied so it must NOT be freed.
 *
 * @returns a shallow copy of the underlying list.
 */
GList *
hash_list_list(hash_list_t *hl)
{
	GList *l = NULL;
	link_t *lk;

	hash_list_check(hl);

	for (lk = elist_last(&hl->list); lk != NULL; lk = elist_prev(lk)) {
		struct hash_list_item *item = ITEM(lk);

		l = g_list_prepend(l, deconstify_pointer(item->key));
	}

	return l;
}

static hash_list_iter_t *
hash_list_iterator_new(hash_list_t *hl, enum hash_list_iter_direction dir)
{
	hash_list_iter_t *iter;

	hash_list_check(hl);

	WALLOC0(iter);
	iter->magic = HASH_LIST_ITER_MAGIC;
	iter->dir = dir;
	iter->hl = hl;
	iter->stamp = hl->stamp;
	hl->refcount++;
	return iter;
}

/**
 * Get an iterator on the list, positionned before first item.
 * Get items with hash_list_iter_next().
 */
hash_list_iter_t *
hash_list_iterator(hash_list_t *hl)
{
	if (hl != NULL) {
		hash_list_iter_t *iter;

		hash_list_check(hl);
		iter = hash_list_iterator_new(hl, HASH_LIST_ITER_FORWARDS);
		iter->next = elist_first(&hl->list);
		return iter;
	} else {
		return NULL;
	}
}

/**
 * Get an iterator on the list, positionned after last item.
 * Get items with hash_list_iter_previous().
 */
hash_list_iter_t *
hash_list_iterator_tail(hash_list_t *hl)
{
	if (hl) {
		hash_list_iter_t *iter;

		hash_list_check(hl);
		iter = hash_list_iterator_new(hl, HASH_LIST_ITER_BACKWARDS);
		iter->prev = elist_last(&hl->list);
		return iter;
	} else {
		return NULL;
	}
}

/**
 * Get an iterator on the list, positionned at the specified item.
 * Get next items with hash_list_iter_next() or hash_list_iter_previous().
 *
 * @return the iterator object or NULL if the key is not in the list.
 */
hash_list_iter_t *
hash_list_iterator_at(hash_list_t *hl, const void *key)
{
	if (hl) {
		struct hash_list_item *item;

		hash_list_check(hl);

		item = htable_lookup(hl->ht, key);
		if (item) {
			hash_list_iter_t *iter;

			iter = hash_list_iterator_new(hl, HASH_LIST_ITER_UNDEFINED);
			iter->prev = elist_prev(&item->lnk);
			iter->next = elist_next(&item->lnk);
			iter->item = item;
			return iter;
		} else {
			return NULL;
		}
	} else {
		return NULL;
	}
}

/**
 * Get the next data item from the iterator, or NULL if none.
 */
G_GNUC_HOT void *
hash_list_iter_next(hash_list_iter_t *iter)
{
	link_t *next;

	hash_list_iter_check(iter);

	next = iter->next;
	if (next != NULL) {
		iter->item = ITEM(next);
		iter->prev = elist_prev(next);
		iter->next = elist_next(next);
		return deconstify_pointer(iter->item->key);
	} else {
		return NULL;
	}
}

/**
 * Checks whether there is a next item to be iterated over.
 */
bool
hash_list_iter_has_next(const hash_list_iter_t *iter)
{
	hash_list_iter_check(iter);

	return NULL != iter->next;
}

/**
 * Get the previous data item from the iterator, or NULL if none.
 */
G_GNUC_HOT void *
hash_list_iter_previous(hash_list_iter_t *iter)
{
	link_t *prev;

	hash_list_iter_check(iter);

	prev = iter->prev;
	if (prev != NULL) {
		iter->item = ITEM(prev);
		iter->next = elist_next(prev);
		iter->prev = elist_prev(prev);
		return deconstify_pointer(iter->item->key);
	} else {
		return NULL;
	}
}

/**
 * Checks whether there is a previous item in the iterator.
 */
bool
hash_list_iter_has_previous(const hash_list_iter_t *iter)
{
	hash_list_iter_check(iter);

	return NULL != iter->prev;
}

/**
 * Checks whether there is a successor in the iterator's direction.
 */
bool
hash_list_iter_has_more(const hash_list_iter_t *iter)
{
	hash_list_iter_check(iter);
	g_assert(iter->dir != HASH_LIST_ITER_UNDEFINED);

	switch (iter->dir) {
	case HASH_LIST_ITER_FORWARDS:
		return hash_list_iter_has_next(iter);
	case HASH_LIST_ITER_BACKWARDS:
		return hash_list_iter_has_previous(iter);
	case HASH_LIST_ITER_UNDEFINED:
		break;
	}
	g_assert_not_reached();
	return FALSE;
}

/**
 * Get the next item in the iterator's direction, NULL if none.
 */
void *
hash_list_iter_move(hash_list_iter_t *iter)
{
	hash_list_iter_check(iter);
	g_assert(iter->dir != HASH_LIST_ITER_UNDEFINED);

	switch (iter->dir) {
	case HASH_LIST_ITER_FORWARDS:
		return hash_list_iter_next(iter);
	case HASH_LIST_ITER_BACKWARDS:
		return hash_list_iter_previous(iter);
	case HASH_LIST_ITER_UNDEFINED:
		break;
	}
	g_assert_not_reached();
	return FALSE;
}

/**
 * Removes current item in the iterator.
 *
 * @return item key, NULL if there is no item to remove.
 */
void *
hash_list_iter_remove(hash_list_iter_t *iter)
{
	struct hash_list_item *item;

	hash_list_iter_check(iter);

	item = iter->item;

	if (item != NULL) {
		void *key = deconstify_pointer(item->key);
		hash_list_t *hl = iter->hl;

		iter->item = NULL;
		htable_remove(hl->ht, key);
		elist_link_remove(&hl->list, &item->lnk);
		WFREE(item);

		return key;
	} else {
		return NULL;
	}
}

/**
 * Release the iterator once we're done with it.
 */
void
hash_list_iter_release(hash_list_iter_t **iter_ptr)
{
	if (*iter_ptr) {
		hash_list_iter_t *iter = *iter_ptr;

		hash_list_iter_check(iter);

		iter->hl->refcount--;
		iter->magic = 0;

		WFREE(iter);
		*iter_ptr = NULL;
	}
}

/**
 * Find key in hashlist.  If ``orig_key_ptr'' is not NULL and the key
 * exists, a pointer to the stored key is written into it.
 *
 * @return TRUE if the key is present.
 */
bool
hash_list_find(hash_list_t *hl, const void *key,
	const void **orig_key_ptr)
{
	struct hash_list_item *item;

	hash_list_check(hl);

	item = htable_lookup(hl->ht, key);
	if (item && orig_key_ptr) {
		*orig_key_ptr = item->key;
	}
	return NULL != item;
}

/**
 * Check whether hashlist contains the key.
 * @return TRUE if the key is present.
 */
bool
hash_list_contains(hash_list_t *hl, const void *key)
{
	hash_list_check(hl);

	return htable_contains(hl->ht, key);
}

/**
 * Get the next item after a given key.
 *
 * This is more costly than taking an iterator and traversing the structure,
 * but it is safe to use when the processing of each item can remove the item
 * from the traversed structure.
 *
 * Here's template code demonstrating usage:
 *
 *		void *next = hash_list_head(hl);
 *		while (next) {
 *			struct <item> *item = next;
 *			next = hash_list_next(hl, next);
 *			<process item, can be safely removed from hl>
 *		}
 *
 * @return pointer to next item, NULL if we reached the end of the list.
 */
void *
hash_list_next(hash_list_t *hl, const void *key)
{
	struct hash_list_item *item;

	hash_list_check(hl);

	item = htable_lookup(hl->ht, key);
	item = item ? elist_data(&hl->list, elist_next(&item->lnk)) : NULL;
	return item ? deconstify_pointer(item->key) : NULL;
}

/**
 * Get the item before a given key.
 */
void *
hash_list_previous(hash_list_t *hl, const void *key)
{
	struct hash_list_item *item;

	hash_list_check(hl);

	item = htable_lookup(hl->ht, key);
	item = item ? elist_data(&hl->list, elist_prev(&item->lnk)) : NULL;
	return item ? deconstify_pointer(item->key) : NULL;
}

/**
 * Apply `func' to all the items in the structure.
 */
void
hash_list_foreach(const hash_list_t *hl, data_fn_t func, void *user_data)
{
	link_t *lk;
	
	hash_list_check(hl);
	g_assert(NULL != func);

	for (lk = elist_first(&hl->list); lk != NULL; lk = elist_next(lk)) {
		struct hash_list_item *item = ITEM(lk);
		(*func)(deconstify_pointer(item->key), user_data);
	}

	hash_list_regression(hl);
}

/**
 * Apply `func' to all the items in the structure, removing the entry
 * if `func' returns TRUE.
 *
 * @return the amount of entries removed from the list.
 */
size_t
hash_list_foreach_remove(hash_list_t *hl, data_rm_fn_t func, void *data)
{
	link_t *lk, *next;
	size_t removed = 0;
	
	hash_list_check(hl);
	g_assert(func != NULL);

	for (lk = elist_first(&hl->list); lk != NULL; lk = next) {
		struct hash_list_item *item = ITEM(lk);

		next = elist_next(lk);
		if ((*func)(deconstify_pointer(item->key), data)) {
			hash_list_remove_item(hl, item);
			removed++;
		}
	}

	hash_list_regression(hl);

	return removed;
}

/* vi: set ts=4 sw=4 cindent: */
