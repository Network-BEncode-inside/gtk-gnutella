/*
 * $Id$
 *
 * Copyright (c) 2007, Christian Biere
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
 * @author Christian Biere
 * @date 2007
 */

#include "common.h"

RCSID("$Id$")

#include "misc.h"
#include "halloc.h"
#include "walloc.h"
#include "lib/sorted_array.h"

#include "override.h"		/* Must be the last header included */

struct sorted_array {
	void *items;		/** The actual array data */
	size_t num_items;	/**< Number of valid items */
	size_t num_size;	/**< Number of allocated items */
	size_t num_added;	/**< Number of items added */
	size_t item_size;	/**< The size of an array item (in bytes) */
	int (*cmp_func)(const void *a, const void *b); /**< Defines the order */
};

struct sorted_array *
sorted_array_new(size_t item_size,
	int (*cmp_func)(const void *a, const void *b))
{
	static const struct sorted_array zero_tab;
	struct sorted_array *tab;

	g_return_val_if_fail(item_size > 0, NULL);
	g_return_val_if_fail(cmp_func, NULL);

	WALLOC(tab);
	*tab = zero_tab;
	tab->item_size = item_size;
	tab->cmp_func = cmp_func;
	return tab;
}

void
sorted_array_free(struct sorted_array **tab_ptr)
{
	struct sorted_array *tab;
	
	tab = *tab_ptr;
	if (tab) {
		HFREE_NULL(tab->items);
		WFREE(tab);
		*tab_ptr = NULL;
	}
}

static inline ALWAYS_INLINE void *
sorted_array_item_intern(const struct sorted_array *tab, size_t i)
{
	char *base = tab->items;
	return &base[tab->item_size * i];
}

void *
sorted_array_item(const struct sorted_array *tab, size_t i)
{
	g_assert(tab);
	g_assert(i < tab->num_items);

	return sorted_array_item_intern(tab, i);
}

G_GNUC_HOT void *
sorted_array_lookup(struct sorted_array *tab, const void *key)
{
	g_assert(tab);

#define GET_ITEM(i) (sorted_array_item_intern(tab, (i)))
#define FOUND(i) G_STMT_START { \
	return sorted_array_item_intern(tab, (i)); \
	/* NOTREACHED */ \
} G_STMT_END
	
	BINARY_SEARCH(const void *, key,
		tab->num_items, tab->cmp_func, GET_ITEM, FOUND);

#undef GET_ITEM
#undef FOUND
	return NULL;
}

void
sorted_array_add(struct sorted_array *tab, const void *item)
{
	void *dst;
	
	g_assert(tab);

	if (tab->num_added >= tab->num_size) {
		tab->num_size = tab->num_size ? (tab->num_size * 2) : 8;
		tab->items = hrealloc(tab->items, tab->num_size * tab->item_size);
	}

	dst = sorted_array_item_intern(tab, tab->num_added);
	memmove(dst, item, tab->item_size);
	tab->num_added++;
}

/**
 * This function must be called after sorted_array_add() to make the
 * changes effective. As this function is costly, it should not be
 * called each time but rather after the complete list of items
 * has been added to the array
 *
 * If collision_func is not NULL, it is used to decide which item will be
 * removed if the array contains multiple equivalent items.
 */
void
sorted_array_sync(struct sorted_array *tab,
	int (*collision_func)(const void *a, const void *b))
{
	size_t i;

	g_assert(tab);

	qsort(tab->items, tab->num_added, tab->item_size, tab->cmp_func);

	/*
	 * Remove duplicates and overlapping ranges. Wider ranges override
	 * narrow ranges.
	 */

	if (collision_func) {
		size_t removed;

		removed = 0;
		for (i = 1; i < tab->num_added; i++) {
			void *a, *b;

			a = sorted_array_item_intern(tab, i - 1);
			b = sorted_array_item_intern(tab, i);
			if (0 == tab->cmp_func(a, b)) {
				void *dst;
				int ret;

				ret = collision_func(a, b);
				if (0 != ret) {
					const void *last;
					
					removed++;
					/* Overwrite the current item with last listed item. */
					last = sorted_array_item_intern(tab,
								tab->num_added - removed);
					dst = ret < 0 ? a : b;
					memcpy(dst, last, tab->item_size);
				}

			}
		}

		if (removed > 0) {
			/* Finally, correct order and item count. */
			tab->num_added -= removed;
			qsort(tab->items, tab->num_added, tab->item_size, tab->cmp_func);
		}
	}

	tab->num_items = tab->num_added;
	
	/* Compact the array if possible to save some memory. */
	if (tab->num_size > tab->num_items) {
		tab->num_size = tab->num_items;
		tab->items = hrealloc(tab->items, tab->num_size * tab->item_size);
	}
}

size_t
sorted_array_size(const struct sorted_array *tab)
{
	g_assert(tab);
	return tab->num_items;
}

/* vi: set ts=4 sw=4 cindent: */
