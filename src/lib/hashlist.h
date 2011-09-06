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

#ifndef _hashlist_h_
#define _hashlist_h_

#include "common.h"
#include "glib-missing.h"

typedef struct hash_list_iter hash_list_iter_t;
typedef struct hash_list hash_list_t;

typedef gboolean (*hashlist_cbr_t)(void *key, void *u);
typedef void (*hashlist_destroy_cb)(void *data);

hash_list_t *hash_list_new(GHashFunc, GEqualFunc);
void hash_list_free(hash_list_t **);
void hash_list_free_all(hash_list_t **hl_ptr, hashlist_destroy_cb freecb);
void *hash_list_remove(hash_list_t *, const void *key);
void *hash_list_remove_head(hash_list_t *);
void *hash_list_remove_tail(hash_list_t *);
void *hash_list_shift(hash_list_t *);
void hash_list_append(hash_list_t *, const void *key);
void hash_list_prepend(hash_list_t *, const void *key);
void hash_list_insert_sorted(hash_list_t *, const void *key, GCompareFunc);
void hash_list_moveto_head(hash_list_t *, const void *key);
void hash_list_moveto_tail(hash_list_t *, const void *key);
void *hash_list_head(const hash_list_t *);
void *hash_list_tail(const hash_list_t *);
void *hash_list_next(hash_list_t *, const void *key);
void *hash_list_previous(hash_list_t *, const void *key);
void hash_list_clear(hash_list_t *hl);
unsigned hash_list_length(const hash_list_t *);
GList *hash_list_list(hash_list_t *);
void hash_list_sort(hash_list_t *, GCompareFunc);
void hash_list_sort_with_data(hash_list_t *, GCompareDataFunc, void *);

hash_list_iter_t *hash_list_iterator(hash_list_t *);
hash_list_iter_t *hash_list_iterator_tail(hash_list_t *);
hash_list_iter_t *hash_list_iterator_at(hash_list_t *, const void *key);
void hash_list_iter_release(hash_list_iter_t **);
gboolean hash_list_iter_has_next(const hash_list_iter_t *) G_GNUC_PURE;
gboolean hash_list_iter_has_previous(const hash_list_iter_t *) G_GNUC_PURE;
gboolean hash_list_iter_has_more(const hash_list_iter_t *iter) G_GNUC_PURE;
void *hash_list_iter_next(hash_list_iter_t *);
void *hash_list_iter_previous(hash_list_iter_t *);
void *hash_list_iter_move(hash_list_iter_t *iter);
void *hash_list_iter_remove(hash_list_iter_t *iter);

gboolean hash_list_find(hash_list_t *, const void *key, const void **orig_key);
gboolean hash_list_contains(hash_list_t *, const void *key);
void hash_list_foreach(const hash_list_t *, GFunc, void *user_data);
size_t hash_list_foreach_remove(hash_list_t *hl, hashlist_cbr_t func, void *u);

void *hash_list_remove_position(hash_list_t *hl, const void *key);
void hash_list_insert_position(hash_list_t *hl, const void *key, void *pos);
void hash_list_forget_position(void *position);

static inline hashlist_destroy_cb
cast_to_hashlist_destroy(func_ptr_t fn)
{
	return (hashlist_destroy_cb) fn;
}

#endif	/* _hashlist_h_ */
