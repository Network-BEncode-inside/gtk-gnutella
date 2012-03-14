/*
 * Copyright (c) 2012 Raphael Manfredi
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
 * Embedded lists are created when the linking pointers are directly
 * held within the data structure, as opposed to glib's lists which are
 * containers pointing at objects.
 *
 * Embedded lists are intrusive in the sense that the objects have an explicit
 * link_t field, but this saves one pointer per item being linked compared
 * to glib's lists.  On the other hand, if an object may belong to several lists
 * based on certain criteria, but not all of the lists (and without any mutual
 * exclusion), then the embedded list approach is not necessarily saving space
 * since some items will have link_t fields that do not get used.
 *
 * Due to the nature of the data structure, the definition of the internal
 * structures is visible in the header file but users must refrain from
 * peeking and poking into the structures.  Using embedded data structures
 * requires more discipline than opaque data structures.
 *
 * The API of embedded lists mirrors that of glib's lists to make a smooth
 * transition possible and maintain some consistency in the code.  That said,
 * the glib list API is quite good so mirroring it is not a problem.
 *
 * @author Raphael Manfredi
 * @date 2012
 */

#include "common.h"

#include "elist.h"
#include "random.h"
#include "unsigned.h"
#include "xmalloc.h"

#include "override.h"			/* Must be the last header included */

/**
 * Initialize embedded list.
 *
 * Assuming items in the list are defined as:
 *
 *     struct item {
 *         <data fields>
 *         link_t lk;
 *     };
 *
 * then the last argument can be given as:
 *
 *     offsetof(struct item, lk)
 *
 * to indicate the place of the node field within the item.
 *
 * @param list		the list structure to initialize
 * @param offset	the offset of the embedded link field within items
 */
void
elist_init(elist_t *list, size_t offset)
{
	g_assert(list != NULL);
	g_assert(size_is_non_negative(offset));

	list->magic = ELIST_MAGIC;
	list->head = NULL;
	list->tail = NULL;
	list->count = 0;
	list->offset = offset;
}

/**
 * Discard list, makiing the list object invalid.
 */
void
elist_discard(elist_t *list)
{
	elist_check(list);

	list->magic = 0;
}

/**
 * Clear list, forgetting about all the items
 *
 * This does not free any of the items, it just empties the list.
 */
void
elist_clear(elist_t *list)
{
	elist_check(list);

	list->head = list->tail = NULL;
	list->count = 0;
}

static inline void
elist_link_append_internal(elist_t *list, link_t *lk)
{
	if G_UNLIKELY(NULL == list->tail) {
		g_assert(NULL == list->head);
		list->head = list->tail = lk;
		lk->next = lk->prev = NULL;
	} else {
		g_assert(NULL == list->tail->next);
		list->tail->next = lk;
		lk->prev = list->tail;
		lk->next = NULL;
		list->tail = lk;
	}

	list->count++;
}

/**
 * Append new link to the list.
 *
 * This is efficient and does not require a full traversal of the list.
 */
void
elist_link_append(elist_t *list, link_t *lk)
{
	elist_check(list);
	g_assert(lk != NULL);

	elist_link_append_internal(list, lk);
}

/**
 * Append new item with embedded link to the list.
 *
 * This is efficient and does not require a full traversal of the list.
 */
void
elist_append(elist_t *list, void *data)
{
	link_t *lk;

	elist_check(list);
	g_assert(data != NULL);

	lk = ptr_add_offset(data, list->offset);
	elist_link_append_internal(list, lk);
}

static inline void
elist_link_prepend_internal(elist_t *list, link_t *lk)
{
	if G_UNLIKELY(NULL == list->head) {
		g_assert(NULL == list->tail);
		list->head = list->tail = lk;
		lk->next = lk->prev = NULL;
	} else {
		g_assert(NULL == list->head->prev);
		list->head->prev = lk;
		lk->next = list->head;
		lk->prev = NULL;
		list->head = lk;
	}

	list->count++;
}

/**
 * Prepend link to the list.
 */
void
elist_link_prepend(elist_t *list, link_t *lk)
{
	elist_check(list);
	g_assert(lk != NULL);

	elist_link_prepend_internal(list, lk);
}

/**
 * Prepend new item with embedded link to the list.
 */
void
elist_prepend(elist_t *list, void *data)
{
	link_t *lk;

	elist_check(list);
	g_assert(data != NULL);

	lk = ptr_add_offset(data, list->offset);
	elist_link_prepend_internal(list, lk);
}

static inline void
elist_link_remove_internal(elist_t *list, link_t *lk)
{
	g_assert(size_is_positive(list->count));

	if G_UNLIKELY(list->head == lk)
		list->head = lk->next;

	if G_UNLIKELY(list->tail == lk)
		list->tail = lk->prev;

	if (lk->prev != NULL)
		lk->prev->next = lk->next;
	if (lk->next != NULL)
		lk->next->prev = lk->prev;

	lk->next = lk->prev = NULL;
	list->count--;
}

/**
 * Remove link from list.
 *
 * The link must be part of that list.
 */
void
elist_link_remove(elist_t *list, link_t *lk)
{
	elist_check(list);
	g_assert(lk != NULL);

	elist_link_remove_internal(list, lk);
}


/**
 * Remove item with embedded link from list.
 *
 * The item must be part of that list.
 */
void
elist_remove(elist_t *list, void *data)
{
	link_t *lk;

	elist_check(list);
	g_assert(data != NULL);

	lk = ptr_add_offset(data, list->offset);
	elist_link_remove_internal(list, lk);
}

static void
elist_link_insert_before_internal(elist_t *list, link_t *siblk, link_t *lk)
{
	g_assert(size_is_positive(list->count));

	if G_UNLIKELY(list->head == siblk)
		list->head = lk;

	if (siblk->prev != NULL)
		siblk->prev->next = lk;

	lk->prev = siblk->prev;
	lk->next = siblk;
	siblk->prev = lk;

	list->count++;
}

/**
 * Insert link before another one in list.
 *
 * The sibling must already be part of the list, the new link must not.
 */
void
elist_link_insert_before(elist_t *list, link_t *sibling_lk, link_t *lk)
{
	elist_check(list);
	g_assert(sibling_lk != NULL);
	g_assert(lk != NULL);

	elist_link_insert_before_internal(list, sibling_lk, lk);
}

/**
 * Insert item before another one in list.
 *
 * The sibling item must already be part of the list, the data item must not.
 */
void
elist_insert_before(elist_t *list, void *sibling, void *data)
{
	link_t *lk, *siblk;

	elist_check(list);
	g_assert(sibling != NULL);
	g_assert(data != NULL);

	siblk = ptr_add_offset(sibling, list->offset);
	lk = ptr_add_offset(data, list->offset);

	elist_link_insert_before_internal(list, siblk, lk);
}

static void
elist_link_insert_after_internal(elist_t *list, link_t *siblk, link_t *lk)
{
	g_assert(size_is_positive(list->count));

	if G_UNLIKELY(list->tail == siblk)
		list->tail = lk;

	if (siblk->next != NULL)
		siblk->next->prev = lk;

	lk->next = siblk->next;
	lk->prev = siblk;
	siblk->next = lk;

	list->count++;
}

/**
 * Insert link after another one in list.
 *
 * The sibling must already be part of the list, the new link must not.
 */
void
elist_link_insert_after(elist_t *list, link_t *sibling_lk, link_t *lk)
{
	elist_check(list);
	g_assert(sibling_lk != NULL);
	g_assert(lk != NULL);

	elist_link_insert_after_internal(list, sibling_lk, lk);
}

/**
 * Insert item after another one in list.
 *
 * The sibling item must already be part of the list, the data item must not.
 */
void
elist_insert_after(elist_t *list, void *sibling, void *data)
{
	link_t *lk, *siblk;

	elist_check(list);
	g_assert(sibling != NULL);
	g_assert(data != NULL);

	siblk = ptr_add_offset(sibling, list->offset);
	lk = ptr_add_offset(data, list->offset);

	elist_link_insert_after_internal(list, siblk, lk);
}

static inline void
elist_link_replace_internal(elist_t *list, link_t *old, link_t *new)
{
	if G_UNLIKELY(list->head == old)
		list->head = new;

	if G_UNLIKELY(list->tail == old)
		list->tail = new;

	if (old->prev != NULL)
		old->prev->next = new;
	if (old->next != NULL)
		old->next->prev = new;
}

/**
 * Replace a link in the list with another link not already in the list.
 */
void
elist_link_replace(elist_t *list, link_t *old, link_t *new)
{
	elist_check(list);
	g_assert(old != NULL);
	g_assert(new != NULL);

	if G_UNLIKELY(old == new)
		return;

	elist_link_replace_internal(list, old, new);

	*new = *old;
	old->next = old->prev = NULL;
}

/**
 * Replace an item in the list with another item not already in the list.
 *
 * The old item is no longer part of the list after the switching and
 * can be safely freed.
 */
void
elist_replace(elist_t *list, void *old, void *new)
{
	link_t *ol, *nl;

	elist_check(list);
	g_assert(old != NULL);
	g_assert(new != NULL);

	if G_UNLIKELY(old == new)
		return;

	ol = ptr_add_offset(old, list->offset);
	nl = ptr_add_offset(new, list->offset);

	elist_link_replace_internal(list, ol, nl);

	*nl = *ol;
	ol->next = ol->prev = NULL;
}

/**
 * Reverse list.
 */
void
elist_reverse(elist_t *list)
{
	link_t *lk, *next;;

	elist_check(list);

	for (lk = list->head; lk != NULL; lk = next) {
		/* Swap next and prev */
		next = lk->next;
		lk->next = lk->prev;
		lk->prev = next;
	}

	/* Swap head and tail */
	lk = list->head;
	list->head = list->tail;
	list->tail = lk;
}

/**
 * Find item in list, using supplied comparison callback to compare list
 * items with the key we're looking for.
 *
 * The key is usually a "dummy" structure with enough fields set to allow
 * comparisons to be made.
 *
 * @param list		the list
 * @param key		key item to locate
 * @param cmp		comparison function to use
 *
 * @return the found item, or NULL if not found.
 */
void *
elist_find(const elist_t *list, const void *key, cmp_fn_t cmp)
{
	link_t *lk;

	elist_check(list);
	g_assert(key != NULL);
	g_assert(cmp != NULL);

	for (lk = list->head; lk != NULL; lk = lk->next) {
		void *data = ptr_add_offset(lk, -list->offset);
		if (0 == (*cmp)(data, key))
			return data;
	}

	return NULL;
}

/**
 * Iterate over the list, invoking the callback for every data item.
 *
 * It is safe for the callback to destroy the item, however this corrupts
 * the list which must therefore be discarded upon return.
 *
 * @param list		the list
 * @param cb		function to invoke on all items
 * @param data		opaque user-data to pass to callback
 */
void
elist_foreach(const elist_t *list, data_fn_t cb, void *data)
{
	link_t *lk, *next;

	elist_check(list);
	g_return_unless(cb != NULL);

	for (lk = list->head; lk != NULL; lk = next) {
		void *item = ptr_add_offset(lk, -list->offset);
		next = lk->next;		/* Allow callback to destroy item */
		(*cb)(item, data);
	}
}

/**
 * Iterate over the list, invoking the callback for every data item
 * and removing the current item if it returns TRUE.
 *
 * @param list		the list
 * @param cbr		function to invoke to determine whether to remove item
 * @param data		opaque user-data to pass to callback
 *
 * @return amount of removed items from the list.
 */
size_t
elist_foreach_remove(elist_t *list, data_rm_fn_t cbr, void *data)
{
	link_t *lk, *next;
	size_t removed = 0;

	elist_check(list);
	g_return_val_unless(cbr != NULL, 0);

	for (lk = list->head; lk != NULL; lk = next) {
		void *item = ptr_add_offset(lk, -list->offset);
		link_t itemlk;

		/*
		 * The callback can free the item, so we must copy the link first.
		 * The remove operation can then use that copied link.
		 */

		next = lk->next;
		itemlk = *lk;		/* Struct copy */

		if ((*cbr)(item, data)) {
			/* Tweak the list to replace possibly gone ``lk'' */
			elist_link_replace_internal(list, lk, &itemlk);
			elist_link_remove_internal(list, &itemlk);
			removed++;
		}
	}

	return removed;
}

/**
 * Run the merge sort algorithm of the sublist, merging back into list.
 *
 * @return the head of the list
 */
static link_t *
elist_merge_sort(elist_t *list, link_t *sublist, size_t count,
	cmp_data_fn_t cmp, void *data)
{
	link_t *l1, *l2, *l, *prev;
	size_t n1, i;
	link_t head;

	if (count <= 1) {
		g_assert(0 != count || NULL == sublist);
		g_assert(0 == count || NULL == sublist->next);

		return sublist;		/* Trivially sorted */
	}

	/*
	 * Divide and conquer: split the list into two, sort each part then
	 * merge the two sorted sublists.
	 */

	n1 = count / 2;

	for (i = 1, l1 = sublist; i < n1; l1 = l1->next, i++)
		/* empty */;

	l2 = l1->next;			/* Start of second list */
	l1->next = NULL;		/* End of first list with ``n1'' items */

	l1 = elist_merge_sort(list, sublist, n1, cmp, data);
	l2 = elist_merge_sort(list, l2, count - n1, cmp, data);

	/*
	 * We now have two sorted (one-way) lists: ``l1'' and ``l2''.
	 * Merge them into `list', taking care of updating its tail, since
	 * we return the head.
	 */

	l = &head;
	prev = NULL;

	while (l1 != NULL && l2 != NULL) {
		void *d1 = ptr_add_offset(l1, -list->offset);
		void *d2 = ptr_add_offset(l2, -list->offset);
		int c = (*cmp)(d1, d2, data);

		if (c <= 0) {
			l->next = l1;
			l1 = l1->next;
		} else {
			l->next = l2;
			l2 = l2->next;
		}
		l = l->next;
		l->prev = prev;
		prev = l;
	}

	l->next = (NULL == l1) ? l2 : l1;
	l->next->prev = l;

	while (l->next != NULL)
		l = l->next;

	list->tail = l;
	return head.next;
}

/**
 * Sort list in place using a merge sort.
 */
static void
elist_sort_internal(elist_t *list, cmp_data_fn_t cmp, void *data)
{
	elist_check(list);
	g_return_unless(cmp != NULL);

	/*
	 * During merging, we use the list as a one-way list chained through
	 * its next pointers and identified by its head and by its amount of
	 * items (to make sub-splitting faster).
	 *
	 * When we come back from the recursion we merge the two sorted lists
	 * and restore its two-way nature, updating the head/tail pointers.
	 */

	list->head = elist_merge_sort(list, list->head, list->count, cmp, data);
}

/**
 * Sort list according to the comparison function, which takes two items
 * plus an additional opaque argument, meant to be used as context to sort
 * the two items.
 *
 * @param list	the list to sort
 * @param cmp	comparison routine to use (for two items)
 * @param data	additional argument to supply to comparison routine
 */
void
elist_sort_with_data(elist_t *list, cmp_data_fn_t cmp, void *data)
{
	elist_sort_internal(list, cmp, data);
}

/**
 * Sort list according to the comparison function, which compares items.
 *
 * @param list	the list to sort
 * @param cmp	comparison routine to use (for two items)
 */
void
elist_sort(elist_t *list, cmp_fn_t cmp)
{
	elist_sort_internal(list, (cmp_data_fn_t) cmp, NULL);
}

/**
 * Insert item in sorted list at the proper position.
 *
 * @param list	the list into which we insert
 * @param item	the item to insert
 * @param cmp	comparison routine to use (for two items) with extra data
 * @param data	user-supplied data for the comparison routine
 */
static void
elist_insert_sorted_internal(elist_t *list, void *item,
	cmp_data_fn_t cmp, void *data)
{
	link_t *lk, *ln;

	elist_check(list);
	g_assert(item != NULL);
	g_assert(cmp != NULL);

	ln = ptr_add_offset(item, list->offset);

	for (lk = list->head; lk != NULL; lk = lk->next) {
		void *p = ptr_add_offset(lk, -list->offset);
		if ((*cmp)(item, p, data) <= 0)
			break;
	}

	if (NULL == lk) {
		elist_link_append_internal(list, ln);
	} else {
		elist_link_insert_before_internal(list, lk, ln);
	}
}

/**
 * Insert item in sorted list at the proper position, as determined by
 * the item comparison routine, in order to keep the whole list sorted
 * after insertion, using the same comparison criteria.
 *
 * The comparison routine takes an extra user-defined context, to assist
 * in the item comparison.
 *
 * @param list	the list into which we insert
 * @param item	the item to insert
 * @param cmp	comparison routine to use (for two items) with extra data
 * @param data	user-supplied data for the comparison routine
 */
void
elist_insert_sorted_with_data(elist_t *list, void *item,
	cmp_data_fn_t cmp, void *data)
{
	elist_insert_sorted_internal(list, item, cmp, data);
}

/**
 * Insert item in sorted list at the proper position, as determined by
 * the item comparison routine, in order to keep the whole list sorted
 * after insertion, using the same comparison criteria.
 *
 * @param list	the list into which we insert
 * @param item	the item to insert
 * @param cmp	comparison routine to use (for two items)
 */
void
elist_insert_sorted(elist_t *list, void *item, cmp_fn_t cmp)
{
	elist_insert_sorted_internal(list, item, (cmp_data_fn_t) cmp, NULL);
}

/**
 * Given a link, return the item associated with the nth link that follows it,
 * or NULL if there is nothing.  The 0th item is the data associated with
 * the given link.
 *
 * @param list	the list
 * @param lk	the starting link, which must be part of the list
 * @param n		how mnay items to move forward starting from the link
 *
 * @return item at the nth position following the link, NULL if none.
 */
void *
elist_nth_next_data(const elist_t *list, const link_t *lk, size_t n)
{
	link_t *l;

	elist_check(list);
	g_assert(lk != NULL);
	g_assert(size_is_non_negative(n));

	l = elist_nth_next(lk, n);
	return NULL == l ? NULL : ptr_add_offset(l, -list->offset);
}

/**
 * Given a link, return the item associated with the nth link that precedes it,
 * or NULL if there is nothing.  The 0th item is the data associated with
 * the given link.
 *
 * @param list	the list
 * @param lk	the starting link, which must be part of the list
 * @param n		how mnay items to move backwards starting from the link
 *
 * @return item at the nth position preceding the link, NULL if none.
 */
void *
elist_nth_prev_data(const elist_t *list, const link_t *lk, size_t n)
{
	link_t *l;

	elist_check(list);
	g_assert(lk != NULL);
	g_assert(size_is_non_negative(n));

	l = elist_nth_prev(lk, n);
	return NULL == l ? NULL : ptr_add_offset(l, -list->offset);
}

/**
 * Randomly shuffle the items in the list.
 */
void
elist_shuffle(elist_t *list)
{
	link_t *lk;
	link_t **array;
	size_t i;

	elist_check(list);

	if G_UNLIKELY(0 == list->count)
		return;

	/*
	 * To ensure O(n) shuffling, build an array containing all the items,
	 * shuffle that array then recreate the list according to the shuffled
	 * array.
	 */

	array = xmalloc(list->count * sizeof array[0]);

	for (i = 0, lk = list->head; lk != NULL; i++, lk = lk->next) {
		array[i] = lk;
	}

	/*
	 * Shuffle the array in-place using Knuth's modern version of the
	 * Fisher and Yates algorithm.
	 */

	for (i = list->count - 1; i > 0; i--) {
		link_t *tmp;
		size_t j = random_value(i);

		/* Swap i-th and j-th items */
		tmp = array[i];
		array[i] = array[j];		/* i-th item has been chosen */
		array[j] = tmp;
	}

	/*
	 * Rebuild the list.
	 */

	list->head = array[0];
	list->tail = array[list->count - 1];

	lk = list->head;
	lk->prev = NULL;

	for (i = 1; i < list->count; i++) {
		link_t *ln = array[i];

		lk->next = ln;
		ln->prev = lk;
		lk = ln;
	}

	lk->next = NULL;
	xfree(array);
}

/* vi: set ts=4 sw=4 cindent: */
