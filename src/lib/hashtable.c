/*
 * Copyright (c) 2009-2012 Raphael Manfredi <Raphael_Manfredi@pobox.com>
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
 * There are three interesting properties in this hash table:
 *
 * - The items and the internal data structures are allocated out of a
 *   same contiguous memory region (aka the "arena").
 *
 * - Memory for the arena is allocated directly through the VMM layer.
 *
 * - The access interface can be dynamically configured to be thread-safe.
 *
 * As such, this hash table is suitable for being used by low-level memory
 * allocators.
 *
 * @author Raphael Manfredi
 * @date 2009-2012
 * @author Christian Biere
 * @date 2006
 */

#include "common.h"

#include "hashtable.h"
#include "atomic.h"
#include "entropy.h"
#include "hashing.h"
#include "mutex.h"
#include "omalloc.h"
#include "pow2.h"
#include "spinlock.h"
#include "vmm.h"
#include "xmalloc.h"

#include "override.h"		/* Must be the last header included */

#define HASH_ITEMS_BINS			2	/* Initial amount of bins */
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
	mutex_t lock;				/* Lock for thread-safe operations */
	mutex_t external_lock;		/* Lock for external atomic operations */
	size_t num_items;			/* Array length of "items" */
	size_t num_bins;			/* Number of bins */
	size_t bin_bits;			/* Number of bits to fold hashed value to */
	size_t num_held;			/* Number of items actually in the table */
	size_t bin_fill;			/* Number of bins in use */
	hash_fn_t hash;				/* Key hash functions, or NULL */
	eq_fn_t eq;					/* Key equality function, or NULL */
	hash_item_t **bins;			/* Array of bins of size ``num_bins'' */
	hash_item_t *free_list;		/* List of free hash items */
	hash_item_t *items;			/* Array of items */
	/*
	 * Lookup caching.
	 */
	const void *last_key;		/* Last looked-up key */
	hash_item_t *last_item;		/* Last looked-up item (NULL if invalid) */
	size_t last_bin;			/* Last bin where looked-up key belongs */
	/*
	 * Since we use these data structures during tracking, be careful:
	 * if the table is created with the _real variant, it is used by
	 * the tracking code and we must make sure to exercise the VMM
	 * layer specially.
	 */
	unsigned real:1;			/* If TRUE, created as "real" */
	unsigned not_leaking:1;		/* Don't track allocated VMM regions */
	unsigned special:1;			/* Set if structure allocated specially */
	unsigned readonly:1;		/* Set if data structures protected */
	unsigned thread_safe:1;		/* Set if table must be thread-safe */
	unsigned once:1;			/* Object allocated using "once" memory */
	unsigned self_keys:1;		/* Keys are self-representing */
};

/**
 * Avoid complexity attacks on the hash table.
 *
 * A random number is used to perturb the hash value for all the keys so
 * that no attack on the hash table insertion complexity can be made, such
 * as presenting a set of keys that will pathologically make insertions
 * O(n) instead of O(1) on average.
 */
static unsigned hash_offset;

/**
 * Minimal amount of bins we we want (power of two) that can fill up one page.
 */
static size_t hash_min_bins;

/**
 * Initialize hash offset if not already done.
 */
static G_GNUC_COLD void
hash_offset_init(void)
{
	static spinlock_t offset_slk = SPINLOCK_INIT;
	static bool done;

	if G_UNLIKELY(!done) {
		spinlock(&offset_slk);
		if (!done) {
			/* Don't allocate any memory, hence can't call arc4random() */
			hash_offset = entropy_random();
			done = TRUE;
			/* Memory barrier will be done by spinunlock() */
		}
		spinunlock(&offset_slk);
	}
}

static inline void *
hash_vmm_alloc(const struct hash_table *ht, size_t size)
{
#ifdef TRACK_VMM
	if (ht->real || ht->not_leaking)
		return vmm_alloc_notrack(size);
	else
#else
	{
		(void) ht;
		return vmm_alloc(size);
	}
#endif	/* TRACK_VMM */
}

static inline void
hash_vmm_free(const struct hash_table *ht, void *p, size_t size)
{
#ifdef TRACK_VMM
	if (ht->real || ht->not_leaking)
		vmm_free_notrack(p, size);
	else
#else
	{
		(void) ht;
		vmm_free(p, size);
	}
#endif	/* TRACK_VMM */
}

static inline void
hash_mark_real(hash_table_t *ht, bool is_real)
{
	ht->real = booleanize(is_real);
}

static inline void
hash_mark_once(hash_table_t *ht, bool is_once)
{
	ht->once = booleanize(is_once);
}

static inline void
hash_copy_flags(hash_table_t *dest, const hash_table_t *src)
{
	dest->real = src->real;
	dest->not_leaking = src->not_leaking;
}

static inline void
hash_table_check(const struct hash_table *ht)
{
	g_assert(ht != NULL);
	g_assert(HASHTABLE_MAGIC == ht->magic);
	g_assert(ht->num_bins > 0 && ht->num_bins < SIZE_MAX / 2);
}

static inline unsigned
hash_id_key(const void *key)
{
	/*
	 * A naive direct use of the pointer has a much worse distribution,
	 * e.g. only a quarter of the bins are used.
	 */

	return GOLDEN_RATIO_32 * pointer_to_ulong(key);
}

static inline bool
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
	size_t num_bins, hash_fn_t hash, eq_fn_t eq)
{
	size_t i;
	size_t arena;
	size_t items_off;

	g_assert(ht);
	g_assert(num_bins > 1);

	hash_offset_init();

	ht->magic = HASHTABLE_MAGIC;
	ht->num_held = 0;
	ht->bin_fill = 0;
	ht->hash = hash != NULL ? hash : hash_id_key;
	ht->eq = eq != NULL ? eq : hash_id_eq;
	ht->self_keys = booleanize(hash_id_eq == ht->eq);

	/*
	 * Since the arena is going to be held in a VMM page with nothing
	 * else, make sure we're filling the page as much as we can.
	 */

	if G_UNLIKELY(0 == hash_min_bins) {
		size_t n;

		/* No spinlock, at worst we'll do this computation more than once */

		n = compat_pagesize() / (sizeof ht->bins[0] +
			HASH_ITEMS_PER_BIN * sizeof ht->items[0]);
		hash_min_bins = 1 << highest_bit_set(n);

		g_assert(hash_min_bins > 1);
		g_assert(IS_POWER_OF_2(hash_min_bins));
	}

	ht->num_bins = MAX(num_bins, hash_min_bins);
	ht->num_items = ht->num_bins * HASH_ITEMS_PER_BIN;
	ht->bin_bits = highest_bit_set64(ht->num_bins);

	g_assert(IS_POWER_OF_2(ht->num_bins));
	g_assert((1UL << ht->bin_bits) == ht->num_bins);

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
hash_table_new_full(hash_fn_t hash, eq_fn_t eq)
{
	hash_table_t *ht = xpmalloc0(sizeof *ht);

	g_assert(ht);

	hash_table_new_intern(ht, HASH_ITEMS_BINS, hash, eq);
	return ht;
}

hash_table_t *
hash_table_new(void)
{
	return hash_table_new_full(NULL, NULL);
}

/**
 * Checks how many items are currently stored in the hash_table.
 *
 * @param ht the hash_table to check.
 * @return the number of items in the hash_table.
 */
size_t
hash_table_size(const hash_table_t *ht)
{
	hash_table_check(ht);

	if (ht->thread_safe)
		atomic_mb();

	return ht->num_held;
}

/**
 * Synchronize access to hash table if thread-safe.
 */
static inline ALWAYS_INLINE void
ht_synchronize(const hash_table_t *ht)
{
	if (ht->thread_safe) {
		hash_table_t *wht = deconstify_pointer(ht);
		mutex_lock(&wht->lock);
		g_assert(HASHTABLE_MAGIC == ht->magic);
	}
}

#define ht_return(ht,v) G_STMT_START {					\
	if (ht->thread_safe) {								\
		hash_table_t *wht = deconstify_pointer(ht);		\
		mutex_unlock(&wht->lock);						\
	}													\
	return v;											\
} G_STMT_END

#define ht_return_void(ht) G_STMT_START {				\
	if (ht->thread_safe) {								\
		hash_table_t *wht = deconstify_pointer(ht);		\
		mutex_unlock(&wht->lock);						\
	}													\
	return;												\
} G_STMT_END

/**
 * @return amount of memory used by the hash table arena.
 */
size_t
hash_table_arena_memory(const hash_table_t *ht)
{
	size_t ret;

	ht_synchronize(ht);
	ret = round_pagesize(hash_bins_items_arena_size(ht, NULL));
	ht_return(ht, ret);
}

static inline unsigned
hash_key(const hash_table_t *ht, const void *key)
{
	return (*ht->hash)(key) + hash_offset;
}

static inline bool
hash_eq(const hash_table_t *ht, const void *a, const void *b)
{
	return a == b || (*ht->eq)(a, b);
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
	size_t idx;

	hash_table_check(ht);

	/*
	 * Caching of last successful lookup result for self-representing keys.
	 */

	if (ht->last_item != NULL && ht->self_keys && ht->last_key == key) {
		if (bin != NULL)
			*bin = ht->last_bin;
		return ht->last_item;
	}

	/*
	 * Lookup key in the hash table.
	 */

	idx = hashing_fold(hash_key(ht, key), ht->bin_bits);
	item = ht->bins[idx];
	if (bin) {
		*bin = idx;
	}

	for ( /* NOTHING */ ; item != NULL; item = item->next) {
		if G_LIKELY(hash_eq(ht, key, item->key)) {
			if (ht->self_keys) {
				/* Cache successful lookup result */
				hash_table_t *wht = deconstify_pointer(ht);
				wht->last_key = key;
				wht->last_bin = idx;
				wht->last_item = item;
			}
			return item;
		}
	}

	return NULL;
}

/**
 * Iterate over the hashtable, invoking the "func" callback on each item
 * with the additional "data" argument.
 */
void
hash_table_foreach(const hash_table_t *ht, ckeyval_fn_t func, void *data)
{
	size_t i, n;

	hash_table_check(ht);
	g_assert(func != NULL);

	ht_synchronize(ht);

	n = ht->num_held;
	i = ht->num_bins;

	while (i-- > 0) {
		hash_item_t *item;

		for (item = ht->bins[i]; NULL != item; item = item->next) {
			(*func)(item->key, deconstify_pointer(item->value), data);
			n--;
		}
	}
	g_assert(0 == n);

	ht_return_void(ht);
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
	ht_synchronize(ht);

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
	ht->last_item = NULL;	/* Clear lookup cache */

	ht_return_void(ht);
}

/**
 * Empty data structure.
 */
static void
hash_table_reset(hash_table_t *ht)
{
	size_t arena;

	hash_table_check(ht);
	g_assert(!ht->readonly);

	arena = hash_bins_items_arena_size(ht, NULL);

	hash_vmm_free(ht, ht->bins, arena);
	ht->bins = NULL;
	ht->num_bins = 0;
	ht->items = NULL;
	ht->num_held = 0;
	ht->num_items = 0;
	ht->free_list = NULL;
	ht->last_key = NULL;
	ht->last_item = NULL;
}

/**
 * Adds a new item to the hash_table. If the hash_table already contains an
 * item with the same key, the old value is kept and FALSE is returned.
 *
 * @return FALSE if the item could not be added, TRUE on success.
 */
static bool
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
	bool ok;
	ok = hash_table_insert_no_resize(data, key, value);
	g_assert(ok);
}

static void
hash_table_resize(hash_table_t *ht, size_t n)
{
	hash_table_t tmp;

	ZERO(&tmp);
	hash_copy_flags(&tmp, ht);
	hash_table_new_intern(&tmp, n, ht->hash, ht->eq);
	hash_table_foreach(ht, hash_table_resize_helper, &tmp);

	g_assert(ht->num_held == tmp.num_held);

	hash_table_reset(ht);

	ht->bins = tmp.bins;
	ht->items = tmp.items;
	ht->num_bins = tmp.num_bins;
	ht->num_items = tmp.num_items;
	ht->num_held = tmp.num_held;
	ht->bin_fill = tmp.bin_fill;
	ht->bin_bits = tmp.bin_bits;
	ht->free_list = tmp.free_list;
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
bool
hash_table_insert(hash_table_t *ht, const void *key, const void *value)
{
	bool ret;

	hash_table_check(ht);
	ht_synchronize(ht);
	g_assert(!ht->readonly);

	hash_table_resize_on_insert(ht);
	ret = hash_table_insert_no_resize(ht, key, value);
	ht_return(ht, ret);
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

/**
 * Remove item from the hash table.
 *
 * @return TRUE if item was present in the hash table.
 */
bool
hash_table_remove(hash_table_t *ht, const void *key)
{
	hash_item_t *item;
	size_t bin;

	hash_table_check(ht);
	ht_synchronize(ht);
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

		if G_UNLIKELY(item == ht->last_item)
			ht->last_item = NULL;	/* Clear lookup cache */

		safety_assert(!hash_table_lookup(ht, key));

		hash_table_resize_on_remove(ht);

		ht_return(ht, TRUE);
	}
	safety_assert(!hash_table_lookup(ht, key));
	ht_return(ht, FALSE);
}

/**
 * Add key/value tuple to the hash table, replacing any existing key/value.
 */
void
hash_table_replace(hash_table_t *ht, const void *key, const void *value)
{
	hash_item_t *item;

	hash_table_check(ht);
	ht_synchronize(ht);
	g_assert(!ht->readonly);

	item = hash_table_find(ht, key, NULL);
	if (item == NULL) {
		hash_table_insert(ht, key, value);
	} else {
		item->key = key;
		item->value = value;
	}

	ht_return_void(ht);
}

/**
 * Lookup key in the table.
 *
 * @return value associated with the key.
 */
void *
hash_table_lookup(const hash_table_t *ht, const void *key)
{
	hash_item_t *item;
	void *p;

	hash_table_check(ht);
	ht_synchronize(ht);

	item = hash_table_find(ht, key, NULL);
	p = item ? deconstify_pointer(item->value) : NULL;

	ht_return(ht, p);
}

/**
 * Lookup key in the hash table, returning physical pointers to the key/value
 * items into ``kp'' and ``vp'' respectively, if non-NULL.
 *
 * @param ht		the hash table
 * @param key		the key to lookup
 * @param kp		where the key pointer is copied, if non-NULL
 * @param vp		where the value pointer is copied, if non-NULL
 *
 * @return TRUE if item was found.
 */
bool
hash_table_lookup_extended(const hash_table_t *ht,
	const void *key, const void **kp, void **vp)
{
	hash_item_t *item;

	hash_table_check(ht);
	ht_synchronize(ht);

	item = hash_table_find(ht, key, NULL);

	if (NULL == item)
		ht_return(ht, FALSE);

	if (kp)
		*kp = item->key;
	if (vp)
		*vp = deconstify_pointer(item->value);

	ht_return(ht, TRUE);
}

/**
 * Check whether hashlist contains the key.
 *
 * @return TRUE if the key is present.
 */
bool
hash_table_contains(const hash_table_t *ht, const void *key)
{
	bool ret;

	hash_table_check(ht);
	ht_synchronize(ht);

	ret = NULL != hash_table_find(ht, key, NULL);
	ht_return(ht, ret);
}

/**
 * Iterate over the hashtable, invoking the "func" callback on each item
 * with the additional "data" argument and removing the item if the
 * callback returns TRUE.
 *
 * @return the amount of items removed from the table.
 */
size_t
hash_table_foreach_remove(hash_table_t *ht, ckeyval_rm_fn_t func, void *data)
{
	size_t i, n, old_n, removed = 0;

	hash_table_check(ht);
	g_assert(func != NULL);

	ht_synchronize(ht);

	n = old_n = ht->num_held;
	i = ht->num_bins;

	while (i-- > 0) {
		hash_item_t *item, *next = NULL, *prev = NULL;

		for (item = ht->bins[i]; NULL != item; item = next) {
			next = item->next;
			if ((*func)(item->key, deconstify_pointer(item->value), data)) {
				/* Remove the item from the table */
				if (item == ht->bins[i]) {
					if (NULL == next) {
						g_assert(ht->bin_fill > 0);
						ht->bin_fill--;
					}
					ht->bins[i] = next;
				} else {
					g_assert(prev != NULL);
					g_assert(prev->next == item);

					prev->next = next;
				}
				hash_item_free(ht, item);
				ht->num_held--;
				if G_UNLIKELY(item == ht->last_item)
					ht->last_item = NULL;	/* Clear lookup cache */
				removed++;
			} else {
				prev = item;	/* Item was kept, becoming new previous item */
			}
			n--;
		}
	}
	g_assert(0 == n);
	g_assert(old_n == removed + ht->num_held);

	if (removed != 0)
		hash_table_resize_on_remove(ht);

	ht_return(ht, removed);
}

/**
 * Destroy hash table, reclaiming all the space.
 */
void
hash_table_destroy(hash_table_t *ht)
{
	hash_table_check(ht);
	ht_synchronize(ht);
	g_assert(!ht->special);
	g_assert(!ht->once);

	hash_table_reset(ht);
	if (ht->thread_safe) {
		mutex_destroy(&ht->external_lock);
		mutex_destroy(&ht->lock);
	}
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
	ht_synchronize(ht);

	if (!ht->readonly) {
		size_t arena = hash_bins_items_arena_size(ht, NULL);

		ht->readonly = booleanize(TRUE);
		mprotect(ht->bins, round_pagesize(arena), PROT_READ);
	}

	ht_return_void(ht);
}

/**
 * Make hash table writable again.
 */
void
hash_table_writable(hash_table_t *ht)
{
	hash_table_check(ht);
	ht_synchronize(ht);

	if (ht->readonly) {
		size_t arena = hash_bins_items_arena_size(ht, NULL);

		ht->readonly = booleanize(FALSE);
		mprotect(ht->bins, round_pagesize(arena), PROT_READ | PROT_WRITE);
	}

	ht_return_void(ht);
}

/**
 * Mark hash table as being thread-safe.
 *
 * This is a once operation which enables all further operations on the
 * hash table to be protected against concurrent accesses.
 */
void
hash_table_thread_safe(hash_table_t *ht)
{
	hash_table_check(ht);

	if (!ht->thread_safe) {
		mutex_init(&ht->lock);
		mutex_init(&ht->external_lock);
		ht->thread_safe = TRUE;
		atomic_mb();
	}
}

/**
 * Grab a mutex on the hash table to allow a sequence of operations to be
 * atomically conducted.
 *
 * It is possible to lock the table several times as long as each locking
 * is paired with a corresponding unlocking in the execution flow.
 *
 * The table must have been marked thread-safe already.
 */
void
hash_table_lock(hash_table_t *ht)
{
	hash_table_check(ht);
	g_assert(ht->thread_safe);

	mutex_lock(&ht->external_lock);
}

/**
 * Release a mutex on a locked hash table.
 *
 * The table must have been marked thread-safe already and locked by the
 * calling thread.
 */
void
hash_table_unlock(hash_table_t *ht)
{
	hash_table_check(ht);
	g_assert(ht->thread_safe);

	mutex_unlock(&ht->external_lock);
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
		size_t ret;

		hash_table_check(ht);
		ht_synchronize(ht);

		ret = sizeof(*ht) + hash_table_arena_memory(ht);
		ht_return(ht, ret);
	}
}

struct ht_linearize {
	void **array;
	size_t count;
	size_t i;
	bool keys;
};

/**
 * Hash table iterator -- linearize the keys/values.
 */
static void
hash_table_linearize_item(const void *key, void *value, void *data)
{
	struct ht_linearize *htl = data;

	g_assert(htl->i < htl->count);

	htl->array[htl->i++] = htl->keys ? deconstify_pointer(key) : value;
}

/**
 * Linearize the keys/values into dynamically allocated array.
 */
static void **
hash_table_linearize(const hash_table_t *ht, size_t *count, bool keys)
{
	struct ht_linearize htl;

	hash_table_check(ht);
	ht_synchronize(ht);

	htl.count = hash_table_size(ht);
	htl.i = 0;
	htl.keys = keys;

	/*
	 * We want to avoid calling xpmalloc() here, but since xmalloc() can
	 * call walloc() which can create a new zone, we have to be careful.
	 * Indeed, zalloc() uses a hash table to store its zones by size, and
	 * we could be linearizing this specific hash table!
	 *
	 * For most hash tables our logic will have no impact and does not add
	 * significant overhead (just another hash_table_size() call).  However,
	 * it can prevent having to allocate more core since small-enough objects
	 * will be allocated using walloc().
	 */

	XMALLOC_ARRAY(htl.array, htl.count);

	while (hash_table_size(ht) != htl.count) {
		htl.count = hash_table_size(ht);
		XREALLOC_ARRAY(htl.array, htl.count);
	}

	hash_table_foreach(ht, hash_table_linearize_item, &htl);

	g_assert(htl.count == htl.i);

	if (count != NULL)
		*count = htl.count;

	ht_return(ht, htl.array);
}

/**
 * Allocate an array of keys which will have to be freed via xfree().
 *
 * @param ht		the hash table
 * @param count		where amount of allocated items is returned if non-NULL
 *
 * @return array of keys (read-only, since any change in keys could change
 * its hashed value and make the table inconsistent).
 */
const void **
hash_table_keys(const hash_table_t *ht, size_t *count)
{
	return (const void **) hash_table_linearize(ht, count, TRUE);
}

/**
 * Allocate an array of values which will have to be freed via xfree().
 *
 * @param ht		the hash table
 * @param count		where amount of allocated items is returned if non-NULL
 *
 * @return array of values.
 */
void **
hash_table_values(const hash_table_t *ht, size_t *count)
{
	return hash_table_linearize(ht, count, FALSE);
}

/**
 * Compute the clustering factor of the hash table.
 *
 * If there are ``n'' items spread overe ``m'' bins, each bin should have
 * n/m items.
 *
 * We can measure the clustering factor ``c'' by computing for each bin i the
 * value Bi = size(bin #i)^2 / n.  Then c = (Sum Bi) - n/m + 1.
 *
 * If each bin has the theoretical value, Bi = (n/m)^2 / n = n/m^2.
 * Hence c = m * n/m^2 - n/m + 1 = 1.
 *
 * If c > 1, then it means there is clustering occurring, the higher the value
 * the higher the clustering.  If c < 1, then the hash function disperses
 * values more efficiently than a pure random function!
 */
double
hash_table_clustering(const hash_table_t *ht)
{
	size_t i, n, m;
	double c = 0.0;

	hash_table_check(ht);

	ht_synchronize(ht);

	n = ht->num_held;
	m = i = ht->num_bins;

	while (i-- > 0) {
		hash_item_t *item;
		size_t j = 0;

		for (item = ht->bins[i]; NULL != item; item = item->next)
			j++;

		if (j != 0)
			c += j*j / (double) n;
	}

	ht_return(ht, 1.0 + c - n / (0 == m ? 1.0 : (double) m));
}

/**
 * Create "special" hash table, where the object is allocated through the
 * specified allocator.
 */
hash_table_t *
hash_table_new_special_full(const hash_table_alloc_t alloc, void *obj,
	hash_fn_t hash, eq_fn_t eq)
{
	hash_table_t *ht = (*alloc)(obj, sizeof *ht);

	g_assert(ht);

	ZERO(ht);
	ht->special = booleanize(TRUE);
	hash_table_new_intern(ht, HASH_ITEMS_BINS, hash, eq);
	return ht;
}

hash_table_t *
hash_table_new_special(const hash_table_alloc_t alloc, void *obj)
{
	return hash_table_new_special_full(alloc, obj, NULL, NULL);
}

hash_table_t *
hash_table_new_full_not_leaking(hash_fn_t hash, eq_fn_t eq)
{
	hash_table_t *ht = omalloc0(sizeof *ht);

	ht->not_leaking = booleanize(TRUE);
	ht->once = booleanize(TRUE);
	hash_table_new_intern(ht, HASH_ITEMS_BINS, hash, eq);
	return ht;
}

hash_table_t *
hash_table_new_not_leaking(void)
{
	return hash_table_new_full_not_leaking(NULL, NULL);
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

static hash_table_t *
hash_table_new_full_real_using(hash_table_t *ht, bool once,
	hash_fn_t hash, eq_fn_t eq)
{
	g_assert(ht != NULL);

	ZERO(ht);
	hash_mark_real(ht, TRUE);
	hash_mark_once(ht, once);
	hash_table_new_intern(ht, HASH_ITEMS_BINS, hash, eq);
	return ht;
}

hash_table_t *
hash_table_once_new_full_real(hash_fn_t hash, eq_fn_t eq)
{
	hash_table_t *ht = omalloc(sizeof *ht);

	return hash_table_new_full_real_using(ht, TRUE, hash, eq);
}

hash_table_t *
hash_table_new_full_real(hash_fn_t hash, eq_fn_t eq)
{
	hash_table_t *ht = malloc(sizeof *ht);

	return hash_table_new_full_real_using(ht, FALSE, hash, eq);
}

hash_table_t *
hash_table_once_new_real(void)
{
	hash_table_t *ht = omalloc(sizeof *ht);

	return hash_table_new_full_real_using(ht, TRUE, NULL, NULL);
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
	ht_synchronize(ht);
	g_assert(!ht->once);

	hash_table_reset(ht);
	if (ht->thread_safe) {
		mutex_destroy(&ht->external_lock);
		mutex_destroy(&ht->lock);
	}
	ht->magic = 0;
	free(ht);
}

/* vi: set ai ts=4 sw=4 cindent: */
