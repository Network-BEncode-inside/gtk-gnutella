/*
 * Copyright (c) 2012, Raphael Manfredi
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
 * Hash table implementation using closed addressing and linear double hashing.
 *
 * Closed addressing means that collision handling is done by using alternative
 * slots in the table, not by chaining values within the same hash bin (a
 * technique referred to as open addressing).
 *
 * Alternative places in the table are looked at using a second hashing
 * function, hence the "double hashing" name.  Since it is highly unlikely
 * that two distinct hashing functions will have the same collision set,
 * the technique prevents secondary clustering in the table.
 *
 * Keys can be classified into four distinct categories:
 *
 * 1. pointers and other stand-for-themselves integer values
 * 2. strings and other NUL-terminated buffers
 * 3. fixed-size buffers
 * 4. complex structures whose hashing depends on its field values.
 *
 * The first 3 classes can be handled natively by this implementation, i.e.
 * the set of hashing routines / equality routines are supplied for the user.
 * But this can be overriden, pretending that the key is of the 4th type.
 *
 * The 4th class requires that two hashing functions and an equality function
 * be supplied, although the hash table can do with one hashing function only,
 * the second hashing being done based on the hash value.  This means that
 * secondary clustering is possible when the hash function collides easily,
 * since two keys hashing with to the same value will have a similar lookup
 * path within the table.
 *
 * For practical reasons and because double hashing is by design relatively
 * immune to clustering, the hash table size is a power of 2: the modulo
 * operator is much slower than a simple trailing bit masking.  To avoid losing
 * the dispertion of the original hashed value, we do not mask the output
 * of the hash function but fold all the bits down to the smaller value.
 *
 * When a collision occurs and alternative slots are to be used, the distance
 * from the "home" slot is computed via the secondary hash routine, and then
 * linear probing of slots is done, using the computed distance.  To guarantee
 * that all the slots will be visited, the distance must be prime with the
 * hash table size.  Because the table size is a power of 2, any odd number
 * will be prime with it.  This is enough to guarantee that for any odd number
 * "d" and a hash table size "S", the first "n", n > 1, for which n*d = 0
 * modulo S is when n = S since d and S are prime to each-other.
 *
 * Because the final position of an item depends on the presence of other
 * items in the traversed path (in case the item is not at its "home" slot),
 * deletion of an item must not mark the slot as free, but must remember
 * that there used to be an item them.  This is done by erecting a tombstone
 * when the item is deleted.
 *
 * Too many items in the table or too many tombstones can affect the overall
 * performance, by increasing the amount of probed slots before finding a value
 * or determining that the value is not present.  Including tombstones, let
 * "r" be the fill ratio of the table, i.e. the number of occupied slots
 * divided by the hash table size.  The theoretical amount of probes done is:
 *
 *     Successful lookup:   -ln(1 - r) / r     (2.01 for r = 0.75)
 *     Unsuccessful lookup: 1 / (1 - r)        (4 for r = 0.75)
 *
 * To keep the performances reasonable, the hash table must therefore be
 * resized when its load factor reaches 0.75, or shrinked (or simply rebuilt)
 * when there are more than, say, 25% tombstones present.
 *
 * In order to accelerate table rebuilds, we keep a copy of the hashed value
 * of every key in the table.  This allows more efficient key comparisons
 * during lookups (keys can't match if they don't have an identical hash) and
 * also allows for flagging empty slots and tombstones, at the cost of
 * reserving two hash values for that purpose: 0 and 1.
 *
 * The code contained here allows for hash tables and hash sets.  Most of the
 * logic is shared, but the API for iteration and insertion is slightly
 * different given that there is no value associated with a key within a set,
 * and the vocabulary is different (we speak of set "items", not "keys").
 *
 * @author Raphael Manfredi
 * @date 2012
 */

#include "common.h"

#define HASH_SOURCE

#include "hash.h"
#include "entropy.h"
#include "hashing.h"
#include "unsigned.h"
#include "vmm.h"
#include "walloc.h"

#include "override.h"			/* Must be the last header included */

#define HASH_HOPS_MAX	8		/* Force resizing if reaching that many hops */

/**
 * Type of table resizing we want to perform.
 */
enum hash_resize_mode {
	HASH_RESIZE_SAME,			/* Rebuild table, no size change */
	HASH_RESIZE_GROW,			/* Grow table */
	HASH_RESIZE_SHRINK,			/* Shrink table */

	HASH_RESIZE_MAXMODE
};

static inline void
hash_check(const void * const p)
{
	const struct hash *h = p;

	g_assert(h != NULL);
	g_assert(HTABLE_MAGIC == h->magic || HSET_MAGIC == h->magic);
}

/**
 * Avoid complexity attacks on the hash table.
 *
 * Random numbers are used to perturb the hash values for all the keys
 * so that no attack on the hash table insertion complexity can be made.
 */
static unsigned hash_offset_primary;
static unsigned hash_offset_secondary;

/**
 * Initialize random hash offset if not already done.
 */
static G_GNUC_COLD void
hash_random_offset_init(void)
{
	static bool done;

	if G_UNLIKELY(!done) {
		hash_offset_primary = entropy_random();
		hash_offset_secondary = entropy_random();
		done = TRUE;
	}
}

/**
 * Compute the total size of the arena required for given amount of items.
 */
static size_t
hash_arena_size(size_t items, bool has_values)
{
	size_t size;

	/*
	 * To avoid too much fragmentation, we allocate the arena as a contiguous
	 * memory region, using walloc() or vmm_alloc() as appropriate.
	 *
	 * When the hash has keys, the layout in memory is:
	 *
	 *     key array | value array | hashes array
	 *
	 * When the has has no keys, the layout is:
	 *
	 *     key array | hashes array
	 *
	 * This allows the hashes array to be correctly aligned since the size
	 * of a pointer is always larger or equal to the size of an unsigned value.
	 */

	STATIC_ASSERT(sizeof(void *) >= sizeof(unsigned));

	size = items * sizeof(void *);
	if (has_values)
		size *= 2;
	size += items * sizeof(unsigned);

	return size;
}

/**
 * Allocate arena for the hash.
 *
 * @param hk		the keyset structure
 * @param bits		log2(size) for the key array
 */
void
hash_arena_allocate(struct hash *h, size_t bits)
{
	struct hkeys *hk = &h->kset;
	size_t size;
	void *arena;

	hash_check(h);
	g_assert(bits >= HASH_MIN_BITS);

	hash_random_offset_init();

	hk->size = 1UL << bits;
	hk->bits = bits;
	hk->tombs = 0;
	hk->resize = FALSE;

	/*
	 * If the arena size is more than a page size, use VMM to allocate the
	 * memory, otherwise rely on walloc().
	 */

	size = hash_arena_size(hk->size, hk->has_values);

	if (size >= round_pagesize(1))
		arena = vmm_alloc(size);
	else
		arena = walloc(size);

	hk->keys = arena;
	arena = ptr_add_offset(arena, hk->size * sizeof(void *));
	if (hk->has_values) {
		(*h->ops->set_values)(h, arena);
		arena = ptr_add_offset(arena, hk->size * sizeof(void *));
	}
	hk->hashes = arena;
	memset(hk->hashes, 0, hk->size * sizeof(unsigned));
}

/**
 * Release arena of given length.
 */
static void
hash_arena_size_free(void *arena, size_t len)
{
	/*
	 * If the arena size is more than a page size, we used VMM to allocate the
	 * memory, otherwise it was walloc()'ed.
	 */

	if (len >= round_pagesize(1))
		vmm_free(arena, len);
	else
		wfree(arena, len);
}

/**
 * Free allocated arena structures.
 */
void
hash_arena_free(struct hash *h)
{
	struct hkeys *hk = &h->kset;
	size_t size;

	hash_check(h);

	/*
	 * If the arena size is more than a page size, we used VMM to allocate the
	 * memory, otherwise it was walloc()'ed.
	 */

	size = hash_arena_size(hk->size, hk->has_values);
	hash_arena_size_free(hk->keys, size);
}

/**
 * Setup hashing routines for keys.
 *
 * @param hk		the keyset structure
 * @param ktype		key type
 * @param keysize	expected for HASH_KEY_FIXED to give key size, otherwise 0
 */
void
hash_keyhash_setup(struct hkeys *hk, enum hash_key_type ktype, size_t keysize)
{
	g_assert(hk != NULL);

	hk->type = ktype;

	switch (ktype) {
	case HASH_KEY_SELF:
		hk->hash = pointer_hash;
		hk->hash2 = pointer_hash2;
		hk->uk.eq = NULL;			/* Will use '==' comparison */
		break;
	case HASH_KEY_STRING:
		hk->hash = string_mix_hash;
		hk->hash2 = string_hash;
		hk->uk.eq = string_eq;
		break;
	case HASH_KEY_FIXED:
		hk->hash = NULL;			/* Will use binary_hash() */
		hk->hash2 = NULL;			/* Will use binary_hash2() */
		hk->uk.keysize = keysize;	/* Will use binary_eq() */
		break;
	case HASH_KEY_ANY:
	case HASH_KEY_MAXTYPE:
		g_assert_not_reached();
	}
}

/**
 * Setup hashing routines for keys.
 *
 * @param hk		the keyset structure
 * @param primary	primary hash function (cannot be NULL)
 * @param secondary	secondary hash function (may be NULL)
 * @param eq		key equality function (NULL means '==' checks)
 */
void
hash_keyhash_any_setup(struct hkeys *hk,
	hash_func_t primary, hash_func_t secondary, hash_eq_t eq)
{
	g_assert(hk != NULL);
	g_assert(primary != NULL);

	hk->type = HASH_KEY_ANY;
	hk->hash = primary;
	hk->hash2 = secondary;
	hk->uk.eq = NULL == eq ? pointer_eq : eq;
}

/**
 * Compute primary hash key.
 */
static inline ALWAYS_INLINE unsigned
hash_compute_primary(const struct hkeys *hk, const void *key)
{
	unsigned hv;

	if (HASH_KEY_FIXED == hk->type) {
		hv = binary_hash(key, hk->uk.keysize);
	} else {
		hv = (*hk->hash)(key);
	}

	hv += hash_offset_primary;

	return G_LIKELY(HASH_IS_REAL(hv)) ? hv : hv + HASH_REAL;
}

/**
 * Compute increment used to jump around the table looking for the key.
 *
 * Makes sure the increment is odd so that it is prime with the table size
 * and non-zero at the same time.
 */
static inline unsigned
hash_compute_increment(const struct hkeys *hk, const void *key, unsigned hv)
{
	unsigned hv2;

	if (HASH_KEY_FIXED == hk->type) {
		hv2 = binary_hash2(key, hk->uk.keysize);
	} else if G_UNLIKELY(NULL == hk->hash2) {
		hv2 = GUINT32_SWAP(hv) ^ GOLDEN_RATIO_32;
	} else {
		hv2 = (*hk->hash2)(key);
	}

	hv2 += hash_offset_secondary;

	return (hv2 & 0x1) ? hv2 : ~hv2;
}

/**
 * Compares two keys.
 */
static bool
hash_keyset_equals(const struct hkeys *hk, const void *k1, const void *k2)
{
	switch (hk->type) {
	case HASH_KEY_SELF:
		return k1 == k2;
	case HASH_KEY_STRING:
	case HASH_KEY_ANY:
		return (*hk->uk.eq)(k1, k2);
	case HASH_KEY_FIXED:
		return binary_eq(k1, k2, hk->uk.keysize);
	case HASH_KEY_MAXTYPE:
		break;
	}
	g_assert_not_reached();
}

/**
 * Lookup key in the key set.
 *
 * Because a lookup is the initial operation before one can insert anything,
 * we keep track of some important parameters that are gathered during the
 * lookup operation: the primary hashed value of the key, the index where
 * the key could be inserted at or where the key is located.
 *
 * @param hk		the keyset structure
 * @param key		the key we are looking for
 * @param hashed	where the hashed value is returned (or given if known)
 * @param kidx		where the key was found or can be inserted
 * @param tombidx	index of the first tomb in the lookup path, -1 if none
 * @param known		TRUE if hashed value is known and given initially
 *
 * @return TRUE if key was found with kidx now holding the index of the key,
 * FALSE otherwise with kidx now holding the insertion index for the key.
 */
static bool
hash_keyset_lookup(struct hkeys *hk, const void *key,
	unsigned *hashed, size_t *kidx, size_t *tombidx, bool known)
{
	unsigned hv, inc, ih;
	size_t idx, nidx;
	size_t first_tomb, mask, hops;
	bool found;

	if (known) {
		hv = *hashed;
	} else {
		*hashed = hv = hash_compute_primary(hk, key);
	}
	idx = hashing_fold(hv, hk->bits);
	ih = hk->hashes[idx];

	/*
	 * If the home slot is free, we're done: the key does not exist and
	 * it can be inserted there.
	 */

	if (HASH_IS_FREE(ih)) {
		*kidx = idx;
		if (tombidx != NULL)
			*tombidx = (size_t) -1;
		return FALSE;
	}

	/*
	 * If the home slot contains the key we're looking for, we're done.
	 */

	if (HASH_IS_REAL(ih)) {
		if (ih == hv && hash_keyset_equals(hk, hk->keys[idx], key)) {
			*kidx = idx;
			if (tombidx != NULL)
				*tombidx = (size_t) -1;
			return TRUE;
		}
	}

	/*
	 * We're going to need the secondary hash now.
	 */

	inc = hash_compute_increment(hk, key, hv);
	first_tomb = (size_t) -1;
	found = FALSE;
	mask = hk->size - 1;		/* Size is power of two */

	/*
	 * By design, the hash table can never become full because we're constantly
	 * monitoring its size and resizing it as it grows past a high watermark.
	 * Therefore, we know we have to end up on a free slot or a tomb.
	 *
	 * We may very well loop back to the original home slot though, meaning
	 * the key was nowhere to be found.  We can't go back to the home slot
	 * before we have been through all the other slots in the table since
	 * the increment and the table size are prime with each other.
	 *
	 * Here's the mathematical proof: let S be the table size (a power of 2)
	 * and i be the increment (odd number).
	 *
	 * Lemma: S and i are prime together.
	 * Proof: let d be a common divisor of S and i.  Since S is a power of 2,
	 * then d is of the form 2^k with k a natural number.  Because i is odd,
	 * it cannot be divided by 2, so d is necessary 2^0 = 1.  QED.
	 *
	 * Now that the lemma is established, here's the original proposition proof.
	 * Let x be the initial position index.  Suppose we find a natural number k
	 * such that x + i*k = x (modulo S).  Here k represents the number of
	 * steps after which we are back to the initial position.
	 * 
	 * Because (Z/nZ, +) is a group, each item has on opposite y such that
	 * x + y = 0 (modulo n). Let us write y = -x.  Adding -x to both sides
	 * of our equation leads to i*k = 0 (modulo S).  This means that it can
	 * also be written i*k = a*S, with "a" an integer number, this being an
	 * equation within R, the set of real numbers.  So k = a*(S/i).  Because
	 * S and i are prime together, S/i is not an integer value unless i = S
	 * or i = 1.  The former would mean that i = 0 (modulo S) and i was odd,
	 * so that's impossible, and the latter means trivially that k is a
	 * multiple of S, meaning k = 0 (mod S).  The only other alternatives for
	 * k to be a natural number when S/i is not an integer are a = 0, in which
	 * case k = 0, or a = n*i, in which case k = n*S so k = 0 (mod S).
	 *
	 * Therefore, we have established the only k such that x + i*k = x (mod S)
	 * is k = 0 (mod S).  We won't come back to x before having looped through
	 * all the other slots first. QED
	 *
	 * Our proof does not depend on the value of x, the initial position, hence
	 * we have demonstrated that the while loop below will either look at all
	 * the slots or stop when it reaches a free slot.
	 */

	nidx = (idx + inc) & mask;
	ih = hk->hashes[nidx];
	hops = 1;

	while (!HASH_IS_FREE(ih) && nidx != idx) {
		if (ih == hv && hash_keyset_equals(hk, hk->keys[nidx], key)) {
			found = TRUE;
			break;
		} else if ((size_t) -1 == first_tomb && HASH_IS_TOMB(ih)) {
			first_tomb = nidx;
		}
		nidx = (nidx + inc) & mask;
		ih = hk->hashes[nidx];
		hops++;
	}

	/*
	 * When lookups go through too many hops before ending, flag for a resizing
	 * at the next opportunity.
	 *
	 * The HASH_HOPS_MAX constant is twice as large as the average amount of
	 * hops we can expect in a table whose maximum fill-up rate is 75%, as
	 * enforced by hash_resize_as_needed().
	 *
	 * If we looped back to the initial index, it means the table is full,
	 * which can only mean there are too many tombs and we still have a
	 * small table (there are less tombs than HASH_HOPS_MAX).  Rebuild!
	 */

	if G_UNLIKELY(hops > HASH_HOPS_MAX || nidx == idx)
		hk->resize = TRUE;

	if (tombidx != NULL)
		*tombidx = first_tomb;
	*kidx = (found || (size_t) -1 == first_tomb) ? nidx : first_tomb;

	return found;
}

/**
 * Erect a new tombstone.
 *
 * @return TRUE if we erected a new tombstone, FALSE if there was already one.
 */
bool
hash_keyset_erect_tombstone(struct hkeys *hk, size_t idx)
{
	g_assert(hk != NULL);
	g_assert(size_is_non_negative(idx));
	g_assert(idx < hk->size);

	if G_UNLIKELY(HASH_TOMB == hk->hashes[idx])
		return FALSE;

	hk->hashes[idx] = HASH_TOMB;
	hk->tombs++;
	return TRUE;
}

/**
 * Resize table according to the resizing mode:
 *
 * HASH_RESIZE_SAME		Rebuild table, no size change
 * HASH_RESIZE_GROW		Double the table size
 * HASH_RESIZE_SHRINK	Reduce table size
 */
static void
hash_resize(struct hash *h, enum hash_resize_mode mode)
{
	const void **old_values = NULL, **new_values;
	const void **old_keys, **hk;
	unsigned *old_hashes, *hp;
	size_t old_size, old_arena_size, i, keys;

	hash_check(h);

	old_keys = h->kset.keys;
	old_hashes = h->kset.hashes;
	if (h->kset.has_values)
		old_values = (*h->ops->get_values)(h);
	old_size = h->kset.size;
	old_arena_size = hash_arena_size(old_size, h->kset.has_values);

	switch (mode) {
	case HASH_RESIZE_SAME:
		goto size_computed;
	case HASH_RESIZE_GROW:
		h->kset.bits++;
		goto size_computed;
	case HASH_RESIZE_SHRINK:
		g_assert(size_is_positive(h->kset.bits));
		do {
			h->kset.bits--;
			h->kset.size = 1UL << h->kset.bits;
		} while
			(h->kset.items < h->kset.size / 4 && h->kset.bits > HASH_MIN_BITS);
		goto size_computed;
	case HASH_RESIZE_MAXMODE:
		break;
	}
	g_assert_not_reached();

size_computed:

	hash_arena_allocate(h, h->kset.bits);

	if (old_values != NULL)
		new_values = (*h->ops->get_values)(h);

	keys = 0;	/* For assertion, count keys */

	for (i = 0, hp = old_hashes, hk = old_keys; i < old_size; i++, hp++, hk++) {
		if (HASH_IS_REAL(*hp)) {
			size_t idx;
			bool found;

			found = hash_keyset_lookup(&h->kset, *hk, hp, &idx, NULL, TRUE);
			g_assert(!found);

			keys++;
			h->kset.keys[idx] = *hk;
			h->kset.hashes[idx] = *hp;
			if (old_values != NULL)
				new_values[idx] = old_values[i];
		}
	}

	g_assert(keys == h->kset.items);

	hash_arena_size_free(old_keys, old_arena_size);
}

/**
 * Resize hash table if needed.
 *
 * @return TRUE if resizing occurred, FALSE otherwise.
 */
bool
hash_resize_as_needed(struct hash *h)
{
	hash_check(h);

	/*
	 * Never resize when iterating, since this would perturb the order
	 * of the keys and mess up with the iterator's position tracking.
	 */

	if G_UNLIKELY(0 != h->refcnt)
		return FALSE;

	if (h->kset.items < h->kset.size / 4) {
		if (h->kset.bits > HASH_MIN_BITS) {
			hash_resize(h, HASH_RESIZE_SHRINK);		/* Table is oversized */
			return TRUE;
		}
	} else if (h->kset.items + h->kset.tombs > h->kset.size / 4 * 3) {
		/* Rebuild if less keys than 3/5 of the size, grow otherwise */
		hash_resize(h, (h->kset.items < h->kset.size / 5 * 3) ?
			HASH_RESIZE_SAME : HASH_RESIZE_GROW);
		return TRUE;
	} else if (h->kset.tombs >= h->kset.size / 4) {
		hash_resize(h, HASH_RESIZE_SAME);		/* Remove tombstones */
		return TRUE;
	} else if (h->kset.resize) {
		if (h->kset.tombs != 0) {
			/* Grow if we won't be attempting to shrink later */
			hash_resize(h, (h->kset.items > h->kset.size / 2) ?
				HASH_RESIZE_GROW : HASH_RESIZE_SAME);
			return TRUE;
		} else if (h->kset.items > h->kset.size / 2) {
			/* Grow if we won't be attempting to shrink later */
			hash_resize(h, HASH_RESIZE_GROW);
			return TRUE;
		} else {
			h->kset.resize = FALSE;		/* Bad luck, live by large hops */
		}
	}

	return FALSE;
}

/**
 * Insert key in table, returning index where insertion was made.
 */
size_t
hash_insert_key(struct hash *h, const void *key)
{
	bool found;
	unsigned hv;
	size_t idx, tombidx;

	hash_check(h);

	hash_resize_as_needed(h);
	found = hash_keyset_lookup(&h->kset, key, &hv, &idx, &tombidx, FALSE);

	if (!found) {
		g_assert(size_is_non_negative(idx));
		g_assert(idx < h->kset.size);

		if (tombidx == idx) {
			g_assert(size_is_positive(h->kset.tombs));
			h->kset.tombs--;
		}
		h->kset.items++;
		h->kset.hashes[idx] = hv;
	}

	h->kset.keys[idx] = key;	/* Could be a new pointer, so always update */

	return idx;
}

/**
 * Lookup key in table, returning index where key was found or -1 if absent.
 */
size_t
hash_lookup_key(struct hash *h, const void *key)
{
	bool found;
	unsigned hv;
	size_t idx, tombidx;

	hash_check(h);

	found = hash_keyset_lookup(&h->kset, key, &hv, &idx, &tombidx, FALSE);

	/*
	 * Regardless of whether key was found, attempt a resize if we went
	 * through too many hops.  If the table ends-up being resized, then
	 * we'll have to look the key again if it was initially present.
	 */

	if G_UNLIKELY(h->kset.resize) {
		if (!hash_resize_as_needed(h))
			goto no_resize;

		if (found) {
			bool kept;

			/* This time we know the key's hash value in ``hv'' */

			kept = hash_keyset_lookup(&h->kset, key, &hv, &idx, &tombidx, TRUE);
			g_assert(kept);		/* Since key existed before resizing */
		}
	}

no_resize:

	if (found) {
		/*
		 * Optimize search path for future lookups of this key: if the lookup
		 * path stepped over a tomb, move the key to that new index and
		 * erect a tombstone at the old location.
		 *
		 * When the reference count is not 0, we don't optimize because they
		 * are iterating on the hash table and therefore we cannot disrupt
		 * the order of the keys.
		 */

		if G_UNLIKELY((size_t) -1 != tombidx && 0 == h->refcnt) {
			const void **values = (*h->ops->get_values)(h);

			g_assert(tombidx != idx);
			g_assert(size_is_positive(h->kset.tombs));

			h->kset.keys[tombidx] = h->kset.keys[idx];
			h->kset.hashes[tombidx] = hv;
			if (values != NULL)
				values[tombidx] = values[idx];

			h->kset.hashes[idx] = HASH_TOMB;
			return tombidx;
		}

		return idx;
	} else {
		return (size_t) -1;		/* Key not found */
	}
}

/**
 * Delete key from table, returning whether key was found.
 */
bool
hash_delete_key(struct hash *h, const void *key)
{
	bool found;
	unsigned hv;
	size_t idx;

	hash_check(h);

	found = hash_keyset_lookup(&h->kset, key, &hv, &idx, NULL, FALSE);

	if (found) {
		bool erected;

		g_assert(size_is_positive(h->kset.items));

		erected = hash_keyset_erect_tombstone(&h->kset, idx);
		g_assert(erected);
		h->kset.items--;
		hash_resize_as_needed(h);
		return TRUE;
	} else {
		return FALSE;		/* Key not found */
	}
}

/**
 * Remove all items.
 */
void
hash_clear(struct hash *h)
{
	hash_check(h);
	g_assert(0 == h->refcnt);

	if G_UNLIKELY(HASH_MIN_BITS == h->kset.bits) {
		memset(h->kset.hashes, 0, h->kset.size * sizeof h->kset.hashes[0]);
		h->kset.tombs = 0;
		h->kset.resize = FALSE;
	} else {
		hash_arena_free(h);
		hash_arena_allocate(h, HASH_MIN_BITS);
	}

	h->kset.items = 0;
}

/**
 * Increase iterator reference count.
 */
void
hash_refcnt_inc(const struct hash *h)
{
	struct hash *wh = deconstify_pointer(h);

	hash_check(h);

	wh->refcnt++;
}

/**
 * Decrease iterator reference count.
 */
void
hash_refcnt_dec(const struct hash *h)
{
	struct hash *wh = deconstify_pointer(h);

	hash_check(h);
	g_assert(size_is_positive(h->refcnt));

	wh->refcnt--;
}

/**
 * Polymorphic traversal, invoking callback for each key.
 *
 * @param h		the hash table
 * @param fn	callback to invoke on the key
 * @param data	additional callback parameter
 */
void
hash_foreach(const struct hash *h, hash_each_key_t fn, void *data)
{
	unsigned *hp, *end;
	size_t i, n;

	hash_check(h);

	end = &h->kset.hashes[h->kset.size];
	hash_refcnt_inc(h);			/* Prevent any key relocation */

	for (i = n = 0, hp = h->kset.hashes; hp != end; i++, hp++) {
		if (HASH_IS_REAL(*hp)) {
			(*fn)(deconstify_pointer(h->kset.keys[i]), data);
			n++;
		}
	}

	g_assert(n == h->kset.items);

	hash_refcnt_dec(h);
}

/**
 * @return amount of itemsi in the hash table/set.
 */
size_t
hash_count(const struct hash *h)
{
	hash_check(h);

	return h->kset.items;
}

/**
 * Free hash structure.
 */
void
hash_free(struct hash *h)
{
	hash_check(h);

	(*h->ops->hash_free)(h);
}

/* vi: set ts=4 sw=4 cindent: */
