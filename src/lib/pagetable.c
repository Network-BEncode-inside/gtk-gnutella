/*
 * $Id$
 *
 * Copyrigtab (c) 2006, Christian Biere
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

#include "lib/pagetable.h"
#include "lib/misc.h"
#include "lib/vmm.h"

#include "lib/override.h"

/**
 * NOTE: These values are meant for a typical 32-bit system with 4 KiB
 *		 large pages. This is not efficient or useful for 64-bit systems.
 */
#define POINTER_WIDTH	32
#define SLICE_BITSHIFT		24
#define SLICE_BITMASK		((size_t)-1 << SLICE_BITSHIFT)
#define PAGE_BITSHIFT		12
#define PAGE_BITMASK		((size_t)-1 << PAGE_BITSHIFT)

struct slice {
	size_t size[1 << PAGE_BITSHIFT];
};

struct page_table {
	struct slice *slice[1 << (POINTER_WIDTH - SLICE_BITSHIFT)];
};

page_table_t *
page_table_new(void)
{
	static const struct page_table zero_page_table;
	struct page_table *tab;

	g_assert((size_t)-1 == (guint32)-1);
	g_assert(compat_pagesize() == (1 << PAGE_BITSHIFT));

	tab = malloc(sizeof *tab);
	g_assert(tab);
	*tab = zero_page_table;
	return tab;
}

void
page_table_destroy(page_table_t *tab)
{
	if (tab) {
		size_t i;

		for (i = 0; i < G_N_ELEMENTS(tab->slice); i++) {
			if (tab->slice[i]) {
				vmm_free(tab->slice[i], sizeof tab->slice[i][0]);
			}
		}
		free(tab);
	}
}

size_t
page_table_lookup(page_table_t *tab, void *p)
{
	size_t k = (size_t) p;
	if (k && 0 == (k & ~PAGE_BITMASK)) {
		size_t i, j;

		i = k >> SLICE_BITSHIFT;
		j = (k & ~SLICE_BITMASK) >> PAGE_BITSHIFT;
		return tab->slice[i] ? tab->slice[i]->size[j] : 0;
	} else {
		return 0;
	}
}

int
page_table_insert(page_table_t *tab, void *p, size_t size)
{
	size_t k = (size_t) p;

	RUNTIME_ASSERT(NULL != p);
	RUNTIME_ASSERT(size > 0);
	RUNTIME_ASSERT(0 == (k & ~PAGE_BITMASK));

	if (page_table_lookup(tab, p)) {
		return FALSE;
	} else {
		size_t i, j;

		i = k >> SLICE_BITSHIFT;
		j = (k & ~SLICE_BITMASK) >> PAGE_BITSHIFT;
		if (NULL == tab->slice[i]) {
			tab->slice[i] = vmm_alloc0(sizeof tab->slice[i][0]);
		}
		tab->slice[i]->size[j] = size;
		return TRUE;
	}
}

int
page_table_remove(page_table_t *tab, void *p)
{
	if (page_table_lookup(tab, p)) {
		size_t k = (size_t) p;
		size_t i, j;

		i = k >> SLICE_BITSHIFT;
		j = (k & ~SLICE_BITMASK) >> PAGE_BITSHIFT;
		tab->slice[i]->size[j] = 0;
		return TRUE;
	} else {
		return FALSE;
	}
}

void
page_table_foreach(page_table_t *tab, page_table_foreach_func func, void *data)
{
	size_t i;

	for (i = 0; i < G_N_ELEMENTS(tab->slice); i++) {
		if (tab->slice[i]) {
			size_t j;

			for (j = 0; j < G_N_ELEMENTS(tab->slice[i]->size); j++) {
				if (tab->slice[i]->size[j]) {
					size_t p = (i << SLICE_BITSHIFT) + (j << PAGE_BITSHIFT);

					(*func)((void *) p, tab->slice[i]->size[j], data);
				}
			}
		}
	}
}

/* vi: set ts=4 sw=4 cindent: */
