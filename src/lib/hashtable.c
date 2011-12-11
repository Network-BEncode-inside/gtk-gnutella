/*
 * Copyright (c) 2009 Raphael Manfredi <Raphael_Manfredi@pobox.com>
 * All rights reserved.
 *
 * Copyright (c) 2006 Christian Biere <christianbiere@gmx.de>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the authors nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/**
 * @ingroup lib
 * @file
 *
 * A simple hashtable implementation.
 *
 * @author Raphael Manfredi
 * @date 2009
 * @author Christian Biere
 * @date 2006
 */

#include "common.h"

#include "lib/hashtable.h"
#include "lib/vmm.h"
#include "lib/xmalloc.h"

#include "lib/override.h"		/* Must be the last header included */

#define HASH_ITEMS_PER_BIN		4
#define HASH_ITEMS_GROW			56

#if 0
#define HASH_TABLE_CHECKS
#endif

/*
 * More costsly run-time assertions are only enabled if HASH_TABLE_CHECKS
 * is defined.
 */
#ifdef HASH_TABLE_CHECKS
#define safety_assert(x)	g_assert(x)
#else
#define safety_assert(x)
#endif

typedef struct hash_item {
	const void *key;
	const void *value;
	struct hash_item *next;
} hash_item_t;

enum hashtable_magic { HASHTABLE_MAGIC = 0x54452ad4 };

struct hash_table {
	enum hashtable_magic magic; /* Magic number */
	size_t num_items;			/* Array length of "items" */
	size_t num_bins;			/* Number of bins */
	size_t num_held;			/* Number of items actually in the table */
	size_t bin_fill;			/* Number of bins in use */
	hash_table_hash_func hash;	/* Key hash functions, or NULL */
	hash_table_eq_func eq;		/* Key equality function, or NULL */
	hash_item_t **bins;			/* Array of bins of size ``num_bins'' */
	hash_item_t *free_list;		/* List of free hash items */
	hash_item_t *items;			/* Array of items */
#ifdef TRACK_VMM
	/*
	 * Since we use these data structures during tracking, be careful:
	 * if the table is created with the _real variant, it is used by
	 * the tracking code and we must make sure to exercise the VMM
	 * layer specially.
	 */
	unsigned real:1;			/* If TRUE, created as "real" */
#endif
	unsigned special:1;			/* Set if structure allocated specially */
	unsigned readonly:1;		/* Set if data structures protected */
};

static inline void *
hash_vmm_alloc(const struct hash_table *ht, size_t size)
{
#ifdef TRACK_VMM
	if (ht->real)
		return vmm_alloc_notrack(size);
	else
		return vmm_alloc(size);
#else
	(void) ht;
	return vmm_alloc(size);
#endif
}

static inline void
hash_vmm_free(const struct hash_table *ht, void *p, size_t size)
{
#ifdef TRACK_VMM
	if (ht->real)
		vmm_free_notrack(p, size);
	else
		vmm_free(p, size);
#else
	(void) ht;
	vmm_free(p, size);
#endif
}

static inline void
hash_mark_real(hash_table_t * ht, gboolean is_real)
{
#ifdef TRACK_VMM
	ht->real = booleanize(is_real);
#else
	(void) ht;
	(void) is_real;
#endif
}

static inline void
hash_copy_real_flag(hash_table_t *dest, const hash_table_t *src)
{
#ifdef TRACK_VMM
	dest->real = src->real;
#else
	(void) dest;
	(void) src;
#endif
}

static inline void
hash_table_check(const struct hash_table *ht)
{
	g_assert(ht != NULL);
	g_assert(HASHTABLE_MAGIC == ht->magic);
	g_assert(ht->num_bins > 0 && ht->num_bins < SIZE_MAX / 2);
}

/**
 * NOTE: A naive direct use of the pointer has a much worse distribution e.g.,
 *		 only a quarter of the bins are used.
 */
static inline size_t
hash_id_key(const void *key)
{
	size_t n = (size_t) key;
	return ((0x4F1BBCDCUL * (guint64) n) >> 32) ^ n;
}

static inline gboolean
hash_id_eq(const void *a, const void *b)
{
	return a == b;
}

static hash_item_t *
hash_item_alloc(hash_table_t *ht, const void *key, const void *value)
{
	hash_item_t *item;

	item = ht->free_list;
	g_assert(item);
	ht->free_list = item->next;

	item->key = key;
	item->value = value;
	item->next = NULL;

	return item;
}

static void
hash_item_free(hash_table_t *ht, hash_item_t *item)
{
	g_assert(ht != NULL);
	g_assert(item != NULL);

	item->key = NULL;
	item->value = NULL;
	item->next = ht->free_list;
	ht->free_list = item;
}

/**
 * Compute how much memory we need to allocate to store the bins and the
 * zone for the items. Bins come first, then optional padding, then items.
 *
 * @return the total size needed and the offset within the big chunk where
 * items will start, taking into account memory alignment constraints.
 */
static size_t
hash_bins_items_arena_size(const hash_table_t *ht, size_t *items_offset)
{
	size_t bins = ht->num_bins * sizeof ht->bins[0];
	size_t items = ht->num_items * sizeof ht->items[0];
	size_t align = bins % MEM_ALIGNBYTES;

	if (align != 0)
		align = MEM_ALIGNBYTES - align; /* Padding to align items */

	if (items_offset)
		*items_offset = bins + align;

	return bins + align + items;
}

static void
hash_table_new_intern(hash_table_t *ht,
	size_t num_bins, hash_table_hash_func hash, hash_table_eq_func eq)
{
	size_t i;
	size_t arena;
	size_t items_off;

	g_assert(ht);
	g_assert(num_bins > 1);
	g_assert(!ht->readonly);

	ht->magic = HASHTABLE_MAGIC;
	ht->num_held = 0;
	ht->bin_fill = 0;
	ht->hash = hash ? hash : hash_id_key;
	ht->eq = eq ? eq : hash_id_eq;

	ht->num_bins = num_bins;
	ht->num_items = ht->num_bins * HASH_ITEMS_PER_BIN;

	arena = hash_bins_items_arena_size(ht, &items_off);

	ht->bins = hash_vmm_alloc(ht, arena);
	g_assert(ht->bins);
	g_assert(items_off != 0);

	ht->items = ptr_add_offset(ht->bins, items_off);

	/* Build free list */

	ht->free_list = &ht->items[0];
	for (i = 0; i < ht->num_items - 1; i++) {
		ht->items[i].next = &ht->items[i + 1];
	}
	ht->items[i].next = NULL;

	/* Initialize bins -- all empty */

	for (i = 0; i < ht->num_bins; i++) {
		ht->bins[i] = NULL;
	}

	hash_table_check(ht);
	g_assert(!ht->readonly);
}

hash_table_t *
hash_table_new_full(hash_table_hash_func hash, hash_table_eq_func eq)
{
	hash_table_t *ht = xpmalloc(sizeof *ht);

	g_assert(ht);

	ZERO(ht);
	hash_table_new_intern(ht, 2, hash, eq);
	return ht;
}

hash_table_t *
hash_table_new(void)
{
	return hash_table_new_full(NULL, NULL);
}

/**
 * Checks how many items are currently in stored in the hash_table.
 *
 * @param ht the hash_table to check.
 * @return the number of items in the hash_table.
 */
size_t
hash_table_size(const hash_table_t *ht)
{
	hash_table_check(ht);
	return ht->num_held;
}

/**
 * @return amount of memory used by the hash table arena.
 */
size_t
hash_table_arena_memory(const hash_table_t *ht)
{
	return round_pagesize(hash_bins_items_arena_size(ht, NULL));
}

/**
 * NOTE: A naive direct use of the pointer has a much worse distribution e.g.,
 *		only a quarter of the bins are used.
 */
static inline size_t
hash_key(const hash_table_t *ht, const void *key)
{
	return (*ht->hash)(key);
}

static inline gboolean
hash_eq(const hash_table_t *ht, const void *a, const void *b)
{
	return (*ht->eq)(a, b);
}

/**
 * @param ht a hash_table.
 * @param key the key to look for.
 * @param bin if not NULL, it will be set to the bin number that is or would
 *		  be used for the key. It is set regardless whether the key is in
 *		  the hash_table.
 * @return NULL if the key is not in the hash_table. Otherwise, the item
 *		   associated with the key is returned.
 */
static hash_item_t *
hash_table_find(const hash_table_t *ht, const void *key, size_t *bin)
{
	hash_item_t *item;
	size_t hash;

	hash_table_check(ht);

	hash = hash_key(ht, key) & (ht->num_bins - 1);
	item = ht->bins[hash];
	if (bin) {
		*bin = hash;
	}

	for ( /* NOTHING */ ; item != NULL; item = item->next) {
		if (hash_eq(ht, key, item->key))
			return item;
	}

	return NULL;
}

void
hash_table_foreach(hash_table_t *ht, hash_table_foreach_func func, void *data)
{
	size_t i, n;

	hash_table_check(ht);
	g_assert(func != NULL);

	n = ht->num_held;
	i = ht->num_bins;

	while (i-- > 0) {
		hash_item_t *item;

		for (item = ht->bins[i]; NULL != item; item = item->next) {
			(*func)(item->key, deconstify_gpointer(item->value), data);
			n--;
		}
	}
	g_assert(0 == n);
}

/**
 * Remove all items from hash table.
 */
void
hash_table_clear(hash_table_t *ht)
{
	size_t i;
	size_t n;

	hash_table_check(ht);

	n = ht->num_held;
	i = ht->num_bins;

	while (i-- > 0) {
		hash_item_t *item = ht->bins[i];

		while (item) {
			hash_item_t *next;

			next = item->next;
			hash_item_free(ht, item);
			item = next;
			n--;
		}
		ht->bins[i] = NULL;
	}
	g_assert(0 == n);

	ht->num_held = 0;
	ht->bin_fill = 0;
}

/**
 * Empty data structure.
 */
static void
hash_table_reset(hash_table_t *ht)
{
	size_t arena;

	hash_table_check(ht);

	arena = hash_bins_items_arena_size(ht, NULL);

	hash_vmm_free(ht, ht->bins, arena);
	ht->bins = NULL;
	ht->num_bins = 0;
	ht->items = NULL;
	ht->num_held = 0;
	ht->num_items = 0;
	ht->free_list = NULL;
}

/**
 * Adds a new item to the hash_table. If the hash_table already contains an
 * item with the same key, the old value is kept and FALSE is returned.
 *
 * @return FALSE if the item could not be added, TRUE on success.
 */
static gboolean
hash_table_insert_no_resize(hash_table_t *ht,
	const void *key, const void *value)
{
	hash_item_t *item;
	size_t bin;

	hash_table_check(ht);

	g_assert(key);
	g_assert(value);

	if (hash_table_find(ht, key, &bin)) {
		return FALSE;
	}
	safety_assert(NULL == hash_table_lookup(ht, key));

	item = hash_item_alloc(ht, key, value);
	g_assert(item != NULL);

	if (NULL == ht->bins[bin]) {
		g_assert(ht->bin_fill < ht->num_bins);
		ht->bin_fill++;
	}
	item->next = ht->bins[bin];
	ht->bins[bin] = item;
	ht->num_held++;

	safety_assert(value == hash_table_lookup(ht, key));
	return TRUE;
}

static void
hash_table_resize_helper(const void *key, void *value, void *data)
{
	gboolean ok;
	ok = hash_table_insert_no_resize(data, key, value);
	g_assert(ok);
}

static void
hash_table_resize(hash_table_t *ht, size_t n)
{
	hash_table_t tmp;

	ZERO(&tmp);
	hash_copy_real_flag(&tmp, ht);
	hash_table_new_intern(&tmp, n, ht->hash, ht->eq);
	hash_table_foreach(ht, hash_table_resize_helper, &tmp);
	g_assert(ht->num_held == tmp.num_held);
	hash_table_reset(ht);
	*ht = tmp;
}

static inline void
hash_table_resize_on_remove(hash_table_t *ht)
{
	size_t n;
	size_t needed_bins = ht->num_held / HASH_ITEMS_PER_BIN;

	n = ht->num_bins / 2;

	if (needed_bins + (HASH_ITEMS_GROW / HASH_ITEMS_PER_BIN) >= n)
		return;

	n = MAX(2, n);
	if (n < needed_bins)
		return;

	hash_table_resize(ht, n);
}

static inline void
hash_table_resize_on_insert(hash_table_t *ht)
{
	if (ht->num_held / HASH_ITEMS_PER_BIN < ht->num_bins)
		return;

	hash_table_resize(ht, ht->num_bins * 2);
}

/**
 * Adds a new item to the hash_table. If the hash_table already contains an
 * item with the same key, the old value is kept and FALSE is returned.
 *
 * @return FALSE if the item could not be added, TRUE on success.
 */
gboolean
hash_table_insert(hash_table_t *ht, const void *key, const void *value)
{
	hash_table_check(ht);
	g_assert(!ht->readonly);

	hash_table_resize_on_insert(ht);
	return hash_table_insert_no_resize(ht, key, value);
}

#if 0	/* UNUSED */
void
hash_table_status(const hash_table_t *ht)
{
	fprintf(stderr,
		"hash_table_status:\n"
		"ht=%p\n"
		"num_held=%lu\n"
		"num_bins=%lu\n"
		"bin_fill=%lu\n",
		ht,
		(unsigned long) ht->num_held,
		(unsigned long) ht->num_bins, (unsigned long) ht->bin_fill);
}
#endif	/* UNUSED */

gboolean
hash_table_remove(hash_table_t *ht, const void *key)
{
	hash_item_t *item;
	size_t bin;

	hash_table_check(ht);
	g_assert(!ht->readonly);

	item = hash_table_find(ht, key, &bin);
	if (item) {
		hash_item_t *i;

		i = ht->bins[bin];
		g_assert(i != NULL);
		if (i == item) {
			if (!i->next) {
				g_assert(ht->bin_fill > 0);
				ht->bin_fill--;
			}
			ht->bins[bin] = i->next;
		} else {

			g_assert(i->next != NULL);
			while (item != i->next) {
				g_assert(i->next != NULL);
				i = i->next;
			}
			g_assert(i->next == item);

			i->next = item->next;
		}

		hash_item_free(ht, item);
		ht->num_held--;

		safety_assert(!hash_table_lookup(ht, key));

		hash_table_resize_on_remove(ht);

		return TRUE;
	}
	safety_assert(!hash_table_lookup(ht, key));
	return FALSE;
}

void
hash_table_replace(hash_table_t *ht, const void *key, const void *value)
{
	hash_item_t *item;

	hash_table_check(ht);
	g_assert(!ht->readonly);

	item = hash_table_find(ht, key, NULL);
	if (item == NULL) {
		hash_table_insert(ht, key, value);
	} else {
		item->key = key;
		item->value = value;
	}
}

void *
hash_table_lookup(const hash_table_t *ht, const void *key)
{
	hash_item_t *item;

	hash_table_check(ht);
	item = hash_table_find(ht, key, NULL);

	return item ? deconstify_gpointer(item->value) : NULL;
}

gboolean
hash_table_lookup_extended(const hash_table_t *ht,
	const void *key, const void **kp, void **vp)
{
	hash_item_t *item;

	hash_table_check(ht);
	item = hash_table_find(ht, key, NULL);

	if (item == NULL)
		return FALSE;

	if (kp)
		*kp = item->key;
	if (vp)
		*vp = deconstify_gpointer(item->value);

	return TRUE;
}

/**
 * Check whether hashlist contains the key.
 * @return TRUE if the key is present.
 */
gboolean
hash_table_contains(const hash_table_t *ht, const void *key)
{
	hash_table_check(ht);

	return NULL != hash_table_find(ht, key, NULL);
}

/**
 * Destroy hash table, reclaiming all the space.
 */
void
hash_table_destroy(hash_table_t *ht)
{
	hash_table_check(ht);
	g_assert(!ht->special);

	hash_table_reset(ht);
	ht->magic = 0;
	xfree(ht);
}

/**
 * Destroy hash table, nullifying its pointer.
 */
void
hash_table_destroy_null(hash_table_t **ht_ptr)
{
	hash_table_t *ht = *ht_ptr;

	if (ht != NULL) {
		hash_table_destroy(ht);
		*ht_ptr = NULL;
	}
}

/**
 * Make hash table read-only.
 *
 * Any accidental attempt to change items will cause a memory protection fault.
 */
void
hash_table_readonly(hash_table_t *ht)
{
	hash_table_check(ht);

	if (!ht->readonly) {
		size_t arena = hash_bins_items_arena_size(ht, NULL);

		ht->readonly = booleanize(TRUE);
		mprotect(ht->bins, round_pagesize(arena), PROT_READ);
	}
}

/**
 * Make hash table writable again.
 */
void
hash_table_writable(hash_table_t *ht)
{
	hash_table_check(ht);

	if (ht->readonly) {
		size_t arena = hash_bins_items_arena_size(ht, NULL);

		ht->readonly = booleanize(FALSE);
		mprotect(ht->bins, round_pagesize(arena), PROT_READ | PROT_WRITE);
	}
}

/**
 * Get memory used by the hash table structures, not counting memory used
 * to store the elements themselves but including the size of the arena
 * and that of the hash table object.
 *
 * @return amount of memory used by the hash table.
 */
size_t
hash_table_memory(const hash_table_t *ht)
{
	if (NULL == ht) {
		return 0;
	} else {
		hash_table_check(ht);
		return sizeof(*ht) + hash_table_arena_memory(ht);
	}
}

/**
 * Create "special" hash table, where the object is allocated through the
 * specified allocator.
 */
hash_table_t *
hash_table_new_special_full(const hash_table_alloc_t alloc, void *obj,
	hash_table_hash_func hash, hash_table_eq_func eq)
{
	hash_table_t *ht = (*alloc)(obj, sizeof *ht);

	g_assert(ht);

	ZERO(ht);
	ht->special = booleanize(TRUE);
	hash_table_new_intern(ht, 2, hash, eq);
	return ht;
}

hash_table_t *
hash_table_new_special(const hash_table_alloc_t alloc, void *obj)
{
	return hash_table_new_special_full(alloc, obj, NULL, NULL);
}

/*
 * The hash table is used to keep track of the malloc() and free() operations,
 * so we need special routines to ensure allocation and freeing of memory
 * uses the real routines, not the remapped ones.
 *
 * These *_real() routines must only be called by the malloc tracking code.
 * Other clients of this hash table should use the regular routines, so that
 * their usage is properly tracked.
 */

#undef malloc
#undef free

hash_table_t *
hash_table_new_full_real(hash_table_hash_func hash, hash_table_eq_func eq)
{
	hash_table_t *ht = malloc(sizeof *ht);

	g_assert(ht);

	ZERO(ht);
	hash_mark_real(ht, TRUE);
	hash_table_new_intern(ht, 2, hash, eq);
	return ht;
}

hash_table_t *
hash_table_new_real(void)
{
	return hash_table_new_full_real(NULL, NULL);
}

void
hash_table_destroy_real(hash_table_t *ht)
{
	hash_table_check(ht);

	hash_table_reset(ht);
	ht->magic = 0;
	free(ht);
}

/* vi: set ai ts=4 sw=4 cindent: */
