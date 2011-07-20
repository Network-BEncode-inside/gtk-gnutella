/*
 * $Id$
 *
 * Copyright (c) 2002-2003, Raphael Manfredi
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
 * Atom management.
 *
 * An atom is a single piece of information that is likely to be shared
 * and which is therefore only allocated once: all other instances point
 * to the common object.
 *
 * @author Raphael Manfredi
 * @date 2002-2003
 */

#define ATOMS_SOURCE
#include "common.h"

#include "atoms.h"
#include "endian.h"

RCSID("$Id$")

#if 0
#define PROTECT_ATOMS
#endif

#if 0
#define ATOMS_HAVE_MAGIC
#endif

/*
 * With PROTECT_ATOMS all atoms are mapped read-only so that they cannot
 * be modified accidently. This is achieved through mprotect(). The CPU
 * overhead is significant and there is also some memory overhead but it's
 * sufficiently low for testing/debugging purposes.
 *
 * TODO: Separate the reference counter (refcount) from the atom itself.
 *       This would heavily the reduce the amount of mprotect() calls and
 *       also reduce the memory overhead.
 *		 We could possibly use the value which we insert into the global
 *       hashtable as reference counter.
 */

#ifdef ATOMS_HAVE_MAGIC
typedef enum {
	ATOM_MAGIC = 0x3eeb9a27U
} atom_prot_magic_t;

#define ATOM_MAGIC_CHECK(a) g_assert(ATOM_MAGIC == (a)->magic)
#define ATOM_SET_MAGIC(a) G_STMT_START { (a)->magic = ATOM_MAGIC; } G_STMT_END
#else
#define ATOM_MAGIC_CHECK(a)
#define ATOM_SET_MAGIC(a)
#endif	/* ATOMS_HAVE_MAGIC */


#include "stringify.h"
#include "misc.h"
#include "glib-missing.h"
#include "walloc.h"

#include "override.h"		/* Must be the last header included */

union mem_chunk {
  void *next;
  char align[MEM_ALIGNBYTES];
};

#define ATOM_TYPE_MASK	((size_t) 0x07)

/**
 * Atoms are ref-counted.
 *
 * The reference count is held at the beginning of the data arena.
 * What we return to the outside is the value of atom_arena(), not a
 * pointer to the atom structure.
 */
typedef struct atom {
#ifdef ATOMS_HAVE_MAGIC
	atom_prot_magic_t magic;	/**< Magic should be at the beginning */
#endif /* ATOM_HAVE_MAGIC */
	int refcnt;				/**< Amount of references */
#ifdef TRACK_ATOMS
	GHashTable *get;		/**< Allocation spots */
	GHashTable *free;		/**< Free spots */
#endif
} atom_t;

#define ARENA_OFFSET \
	(MEM_ALIGNBYTES * ((sizeof(atom_t) / MEM_ALIGNBYTES) + \
		((sizeof(atom_t) % MEM_ALIGNBYTES) ? 1 : 0)))

static inline atom_t *
atom_from_arena(gconstpointer key)
{
	return (atom_t *) (((char *) key) - ARENA_OFFSET);
}

static inline void *
atom_arena(const atom_t *a)
{
	return ((char *) a) + ARENA_OFFSET;
}

static inline void
atom_check(const atom_t *a)
{
	g_assert(a);
	ATOM_MAGIC_CHECK(a);
}

#ifdef PROTECT_ATOMS
struct mem_pool {
	union mem_chunk chunks[1];
};

struct mem_cache {
	struct mem_pool **pools; /**< Dynamic array */
	union mem_chunk *avail;	 /**< First available chunk */
	size_t num_pools;		 /**< Element count of ``pools''. */
	size_t size;			 /**< Size of a single chunk */
	size_t hold;
};

static struct mem_cache *mem_cache[9];

static struct mem_pool *
mem_pool_new(size_t size, size_t hold)
{
  struct mem_pool *mp;
  size_t len, ps;
  
  g_assert(size >= sizeof mp->chunks[0]);
  g_assert(0 == size % sizeof mp->chunks[0]);
  g_assert(hold > 0);
  
  ps = compat_pagesize();
  len = hold * size;
  
  mp = vmm_alloc(len);
  if (mp) {
    size_t i, step = size / sizeof mp->chunks[0];
    
	for (i = 0; hold-- > 1; i += step) {
      mp->chunks[i].next = &mp->chunks[i + step];
	}

    mp->chunks[i].next = NULL;
  }

  return mp;
}

/**
 * Allocates a mem cache that can be used to allocate memory chunks of
 * the given size. The cache works as a stack, that means the last freed
 * chunk will be used for the next allocation.
 */
static struct mem_cache *
mem_new(size_t size)
{
  struct mem_cache *mc;

  g_assert(size > 0);
  
  mc = g_malloc(sizeof *mc);
  if (mc) {
    union mem_chunk chunk;
    
    mc->size = round_size(sizeof chunk, size);
    mc->hold = MAX(1, compat_pagesize() / mc->size);
    mc->num_pools = 0;
    mc->avail = NULL;
    mc->pools = NULL;
  }
  return mc;
}

/**
 * Allocate a chunk.
 */
static void *
mem_alloc(struct mem_cache *mc)
{
  void *p;

  g_assert(mc);
  
  if (NULL != (p = mc->avail)) {
    mc->avail = mc->avail->next;
  } else {
    struct mem_pool *mp;
    
    mp = mem_pool_new(mc->size, mc->hold);
    if (mp) {
      void *q;
      
      q = g_realloc(mc->pools, (1 + mc->num_pools) * sizeof mc->pools[0]);
      if (q) {
        mc->pools = q;
        mc->pools[mc->num_pools++] = mp;
      
        mc->avail = &mp->chunks[0];
        p = mc->avail;
        mc->avail = mc->avail->next;
      } else {
        free(mp);
      }
    }
  }

  return p;
}

/**
 * This does not really "free" the chunk at ``p'' but pushes it back to
 * the mem cache and thus makes it available again for mem_alloc(). 
 */
static void
mem_free(struct mem_cache *mc, void *p)
{
  g_assert(mc);

  if (p) {
    union mem_chunk *c = p;
   
    c->next = mc->avail;
    mc->avail = c;
  }
}

/**
 * Find the appropriate mem cache for the given size.
 */
static struct mem_cache *
atom_get_mc(size_t size)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(mem_cache); i++) {
		if (size <= mem_cache[i]->size)
			return mem_cache[i];
	}
	return NULL;
}

/**
 * Maps the memory chunk at ``ptr'' of length ``size'' read-only.
 * @note All pages partially overlapped by the chunk will be affected.
 */
static inline void
mem_protect(gpointer ptr, size_t size)
{
	gpointer p;
	size_t ps;

	ps = compat_pagesize();
	p = (char *) ptr - (uintptr_t) ptr % ps;
	size = round_size(ps, size);

	if (-1 == mprotect(p, size, PROT_READ))
		g_warning("mem_protect: mprotect(%p, %u, PROT_READ) failed: %s",
			p, size, g_strerror(errno));
}

/**
 * Maps the memory chunk at ``ptr'' of length ``size'' read- and writable.
 * @note All pages partially overlapped by the chunk will be affected.
 */
static inline void
mem_unprotect(gpointer ptr, size_t size)
{
	gpointer p;
	size_t ps;

	ps = compat_pagesize();
	p = (char *) ptr - (uintptr_t) ptr % ps;
	size = round_size(ps, size);

	if (-1 == mprotect(p, size, PROT_READ | PROT_WRITE))
		g_warning("mem_unprotect: mprotect(%p, %u, PROT_RDWR) failed: %s",
			p, size, g_strerror(errno));
}

/**
 * Maps an atom read- and writable.
 * @note All atoms that reside in the same page will be affected.
 */
static inline void
atom_unprotect(atom_t *a, size_t size)
{
	atom_check(a);
	mem_unprotect(a, size);
}

/**
 * Maps an atom read-only.
 * @note All atoms that reside in the same page will be affected.
 */
static inline void
atom_protect(atom_t *a, size_t size)
{
	atom_check(a);
	mem_protect(a, size);
}

/**
 * Allocates a new atom with the given size. The returned atom is initially
 * unprotected.
 */
static atom_t *
atom_alloc(size_t size)
{
	struct mem_cache *mc_ptr;
	atom_t *a;

	g_assert(size > 0);

	mc_ptr = atom_get_mc(size);
	if (mc_ptr) {
		a = mem_alloc(mc_ptr);
		mem_unprotect(a, size);
	} else {
		a = vmm_alloc(size);
	}
	ATOM_SET_MAGIC(a);

	return a;
}

/**
 * Virtually frees the given atom. If the atom used a chunk from a mem cache,
 * it is returned to this cache and further mapped read-only. Otherwise, if
 * the atom uses ordinary memory returned by malloc(), the memory is released
 * with free() and not further protected.
 */
static void
atom_dealloc(atom_t *a, size_t size)
{
	struct mem_cache *mc_ptr;

	atom_check(a);

	mc_ptr = atom_get_mc(size);
	if (mc_ptr) {
		memset(a, 0, size);
		mem_free(mc_ptr, a);
		/*
		 * It would be best to map the atom with PROT_NONE but there
		 * may be atoms in use that reside in the same page.
		 */
		mem_protect(a, size);
	} else {
		/* This may cause trouble if any library code (libc for example)
		 * relies on executable heap-memory because the page is now
		 * mapped (PROT_READ | PROT_WRITE).
		 */
		VMM_FREE_NULL(a, size);
	}
}
#else

#define atom_protect(a, size)
#define atom_unprotect(a, size)

static inline atom_t *
atom_alloc(size_t size)
{
	atom_t *a = walloc(size);
	return a;	
}

static inline void
atom_dealloc(atom_t *a, size_t size)
{
	wfree(a, size);
}

#endif /* PROTECT_ATOMS */

typedef size_t (*len_func_t)(gconstpointer v);
typedef const char *(*str_func_t)(gconstpointer v);

/**
 * Description of atom types.
 */
typedef struct table_desc {
	const char *type;			/**< Type of atoms */
	GHashTable *table;			/**< Table of atoms: "atom value" => 1 */
	GHashFunc hash_func;		/**< Hashing function for atoms */
	GCompareFunc eq_func;		/**< Atom equality function */
	len_func_t len_func;		/**< Atom length function */
	str_func_t str_func;		/**< Atom to human-readable string */
} table_desc_t;

static size_t str_len(gconstpointer v);
static const char *str_str(gconstpointer v);
static size_t guid_len(gconstpointer v);
static const char *guid_str(gconstpointer v);
static size_t sha1_len(gconstpointer v);
static const char *sha1_str(gconstpointer v);
static size_t tth_len(gconstpointer v);
static const char *tth_str(gconstpointer v);
static size_t uint64_len(gconstpointer v);
static const char *uint64_str(gconstpointer v);
static size_t filesize_len(gconstpointer v);
static const char *filesize_str(gconstpointer v);
static size_t uint32_len(gconstpointer v);
static const char *uint32_str(gconstpointer v);
static const char *gnet_host_str(gconstpointer v);

/**
 * The set of all atom types we know about.
 */
static table_desc_t atoms[] = {
	{ "String",	NULL, g_str_hash,  g_str_equal, str_len,    str_str  },	/* 0 */
	{ "GUID",	NULL, guid_hash,   guid_eq,	    guid_len,   guid_str },	/* 1 */
	{ "SHA1",	NULL, sha1_hash,   sha1_eq,	    sha1_len,   sha1_str },	/* 2 */
	{ "TTH",	NULL, tth_hash,    tth_eq,	    tth_len,    tth_str },	/* 3 */
	{ "uint64",	NULL, uint64_hash, uint64_eq,   uint64_len, uint64_str},/* 4 */
	{ "filesize", NULL,
		filesize_hash, filesize_eq, filesize_len, filesize_str},		/* 5 */
	{ "uint32",	NULL, uint32_hash, uint32_eq,   uint32_len, uint32_str},/* 6 */
	{ "host", NULL,
		gnet_host_hash, gnet_host_eq, gnet_host_length, gnet_host_str},	/* 7 */
};

static GHashTable *ht_all_atoms;

/**
 * @return length of string + trailing NUL.
 */
static size_t 
str_len(gconstpointer v)
{
	return strlen((const char *) v) + 1;
}

/**
 * @return printable form of a string, i.e. self.
 */
static const char *
str_str(gconstpointer v)
{
	return (const char *) v;
}

/**
 * Hash `len' bytes starting from `data'.
 */
G_GNUC_HOT guint
binary_hash(gconstpointer data, size_t len)
{
	const unsigned char *key = data;
	size_t i, remain, t4;
	guint32 hash;

	remain = len & 0x3;
	t4 = len & ~0x3U;

	g_assert(remain + t4 == len);
	g_assert(remain <= 3);

	hash = len;
	for (i = 0; i < t4; i += 4) {
		static const guint32 x[] = {
			0xb0994420, 0x01fa96e3, 0x05066d0e, 0x50c3c22a,
			0xec99f01f, 0xc0eaa79d, 0x157d4257, 0xde2b8419
		};
		hash ^= peek_le32(&key[i]);
		hash += x[(i >> 2) & 0x7];
		hash = (hash << 24) ^ (hash >> 8);
	}

	for (i = 0; i < remain; i++) {
		hash += key[t4 + i];
		hash ^= key[t4 + i] << (i * 8);
		hash = (hash << 24) ^ (hash >> 8);
	}

	return pointer_hash_func((void *) (size_t) hash);
}

/**
 * Hash a GUID (16 bytes).
 */
guint
guid_hash(gconstpointer key)
{
	return binary_hash(key, GUID_RAW_SIZE);
}

/**
 * Test two GUIDs for equality.
 */
int
guid_eq(gconstpointer a, gconstpointer b)
{
	return a == b || 0 == memcmp(a, b, GUID_RAW_SIZE);
}

/**
 * @return length of GUID.
 */
static size_t 
guid_len(gconstpointer unused_guid)
{
	(void) unused_guid;
	return GUID_RAW_SIZE;
}

/**
 * @return printable form of a GUID, as pointer to static data.
 */
static const char *
guid_str(gconstpointer v)
{
	const struct guid *guid = v;
	return guid_hex_str(guid);
}

/**
 * Hash a SHA1 (20 bytes).
 */
guint
sha1_hash(gconstpointer key)
{
	return binary_hash(key, SHA1_RAW_SIZE);
}

/**
 * Test two SHA1s for equality.
 */
int
sha1_eq(gconstpointer a, gconstpointer b)
{
	return a == b || 0 == memcmp(a, b, SHA1_RAW_SIZE);
}

/**
 * @return length of SHA1.
 */
static size_t 
sha1_len(gconstpointer unused_sha1)
{
	(void) unused_sha1;
	return SHA1_RAW_SIZE;
}

/**
 * @return printable form of a SHA1, as pointer to static data.
 */
static const char *
sha1_str(gconstpointer sha1)
{
	return sha1_base32(sha1);
}

/**
 * Hash a TTH (24 bytes).
 */
guint
tth_hash(gconstpointer key)
{
	return binary_hash(key, TTH_RAW_SIZE);
}

/**
 * Test two TTHs for equality.
 *
 * @attention
 * NB: This routine is visible for the download mesh.
 */
int
tth_eq(gconstpointer a, gconstpointer b)
{
	return a == b || 0 == memcmp(a, b, TTH_RAW_SIZE);
}

/**
 * @return length of TTH.
 */
static size_t 
tth_len(gconstpointer unused_tth)
{
	(void) unused_tth;
	return TTH_RAW_SIZE;
}

/**
 * @return printable form of a TTH, as pointer to static data.
 */
static const char *
tth_str(gconstpointer tth)
{
	return tth_base32(tth);
}

/**
 * @return length of a 64-bit integer.
 */
static size_t
uint64_len(gconstpointer unused_v)
{
	(void) unused_v;
	return sizeof(guint64);
}

/**
 * @return length of a filesize_t.
 */
static size_t
filesize_len(gconstpointer unused_v)
{
	(void) unused_v;
	return sizeof(filesize_t);
}

/**
 * @return length of a 32-bit integer.
 */
static size_t
uint32_len(gconstpointer unused_v)
{
	(void) unused_v;
	return sizeof(guint32);
}

/**
 * @return printable form of a 64-bit integer, as pointer to static data.
 */
static const char *
uint64_str(gconstpointer v)
{
	static char buf[UINT64_DEC_BUFLEN];

	uint64_to_string_buf(*(const guint64 *) v, buf, sizeof buf);
	return buf;
}

/**
 * Test two 64-bit integers for equality.
 *
 * @return whether both referenced 64-bit integers are equal.
 */
int
uint64_eq(gconstpointer a, gconstpointer b)
{
	return *(const guint64 *) a == *(const guint64 *) b;
}

/**
 * Calculate the 32-bit hash of a 64-bit integer
 *
 * @return the 32-bit hash value for the referenced 64-bit integer.
 */
guint
uint64_hash(gconstpointer p)
{
	guint64 v = *(const guint64 *) p;
	return v ^ (v >> 32);
}

/**
 * Test two 64-bit integers for equality, with pointers not necessarily
 * aligned on 64-bit quantities.
 *
 * @return whether both referenced 64-bit integers are equal.
 */
int
uint64_mem_eq(gconstpointer a, gconstpointer b)
{
	return a == b || 0 == memcmp(a, b, sizeof(guint64));
}

/**
 * Calculate the 32-bit hash of a 64-bit integer whose address is not
 * necessarily aligned on 64-bit quantities.
 *
 * @return the 32-bit hash value for the referenced 64-bit integer.
 */
guint
uint64_mem_hash(gconstpointer p)
{
	return binary_hash(p, sizeof(guint64));
}

/**
 * @return printable form of a filesize_t, as pointer to static data.
 */
static const char *
filesize_str(gconstpointer v)
{
	static char buf[UINT64_DEC_BUFLEN];

	uint64_to_string_buf(*(const filesize_t *) v, buf, sizeof buf);
	return buf;
}

/**
 * @return printable form of a gnet_host_t *, as pointer to static data.
 */
static const char *
gnet_host_str(gconstpointer v)
{
	static char buf[HOST_ADDR_PORT_BUFLEN];

	gnet_host_to_string_buf(v, buf, sizeof buf);
	return buf;
}

/**
 * Test two filesize_t for equality.
 *
 * @return whether both referenced 64-bit integers are equal.
 */
int
filesize_eq(gconstpointer a, gconstpointer b)
{
	return *(const filesize_t *) a == *(const filesize_t *) b;
}

/**
 * Calculate the 32-bit hash of a filesize_t.
 *
 * @return the 32-bit hash value for the referenced 64-bit integer.
 */
guint
filesize_hash(gconstpointer p)
{
	guint64 v = *(const filesize_t *) p;
	return v ^ (v >> 32);
}

/**
 * Test two 32-bit integers for equality.
 *
 * @return whether both referenced 32-bit integers are equal.
 */
int
uint32_eq(gconstpointer a, gconstpointer b)
{
	return *(const guint32 *) a == *(const guint32 *) b;
}

/**
 * Calculate the 32-bit hash of a 32-bit integer
 *
 * @return the 32-bit hash value for the referenced 32-bit integer.
 */
guint
uint32_hash(gconstpointer p)
{
	guint32 v = *(const guint32 *) p;
	return v;
}

/**
 * @return printable form of a 32-bit integer, as pointer to static data.
 */
static const char *
uint32_str(gconstpointer v)
{
	static char buf[UINT32_DEC_BUFLEN];

	uint32_to_string_buf(*(const guint32 *) v, buf, sizeof buf);
	return buf;
}

/**
 * Initialize atom structures.
 */
void
atoms_init(void)
{
	gboolean has_setting = FALSE;
	struct atom_settings {
		guint8 track_atoms;
		guint8 protect_atoms;
		guint8 atoms_have_magic;
	} settings;
	guint i;

	ZERO(&settings);

	STATIC_ASSERT(NUM_ATOM_TYPES <= (ATOM_TYPE_MASK + 1));
	STATIC_ASSERT(NUM_ATOM_TYPES == G_N_ELEMENTS(atoms));

#ifdef PROTECT_ATOMS
	for (i = 0; i < G_N_ELEMENTS(mem_cache); i++) {
		mem_cache[i] = mem_new((2 * sizeof (union mem_chunk)) << i);
	}
#endif /* PROTECT_ATOMS */

	for (i = 0; i < G_N_ELEMENTS(atoms); i++) {
		table_desc_t *td = &atoms[i];

		td->table = g_hash_table_new(td->hash_func, td->eq_func);
	}
	ht_all_atoms = g_hash_table_new(pointer_hash_func, NULL);

	/*
	 * Log atoms configuration.
	 */

#ifdef TRACK_ATOMS
	settings.track_atoms = TRUE;
	has_setting = TRUE;
#endif
#ifdef PROTECT_ATOMS
	settings.protect_atoms = TRUE;
	has_setting = TRUE;
#endif
#ifdef ATOMS_HAVE_MAGIC
	settings.atoms_have_magic = TRUE;
	has_setting = TRUE;
#endif

	if (has_setting) {
		g_message("atom settings: %s%s%s",
			settings.track_atoms ? "TRACK_ATOMS " : "",
			settings.protect_atoms ? "PROTECT_ATOMS " : "",
			settings.atoms_have_magic ? "ATOMS_HAVE_MAGIC " : "");
	}
}

/**
 * Check whether ``key'' is an atom of type ``type''.
 *
 * @return the size of the atom if found, 0 otherwise.
 */
static inline size_t
atom_is_registered(enum atom_type type, gconstpointer key)
{
	gpointer value;

	if (g_hash_table_lookup_extended(ht_all_atoms, key, NULL, &value)) {
		/*
		 * If the address is already registered in the global atom table,
		 * this is definitely an atom. However, the same memory object
		 * could be shared by atoms of different types (in theory at least),
		 * thus we must check whether the types are identical.
		 */

		if (((size_t) value & ATOM_TYPE_MASK) == (guint) type) {
			size_t size = (size_t) value & ~ATOM_TYPE_MASK;
			g_assert(size >= ARENA_OFFSET);
			return size;
		}
	}
	return 0;
}

/**
 * Check whether atom exists.
 *
 * @return TRUE if ``key'' points to a ``type'' atom.
 */
gboolean
atom_exists(enum atom_type type, gconstpointer key)
{
	g_assert(key != NULL);

	return atom_is_registered(type, key) > 0 ||
		gm_hash_table_contains(atoms[type].table, key);
}

/**
 * Get atom of given `type', whose value is `key'.
 * If the atom does not exist yet, `key' is cloned and makes up the new atom.
 *
 * @return the atom's value.
 */
gconstpointer
atom_get(enum atom_type type, gconstpointer key)
{
	table_desc_t *td;
	gpointer orig_key;
	size_t size;

	STATIC_ASSERT(0 == ARENA_OFFSET % MEM_ALIGNBYTES);
	STATIC_ASSERT(ARENA_OFFSET >= sizeof(atom_t));
	
    g_assert(key != NULL);
	g_assert(UNSIGNED(type) < G_N_ELEMENTS(atoms));

	td = &atoms[type];		/* Where atoms of this type are held */

	size = atom_is_registered(type, key);
	if (size > 0) {
		/* Atom already exists for key and type */
		orig_key = deconstify_gpointer(key);
	} else {
		gpointer value;

		/*
		 * Normally, if atom exists, it will be found by atom_is_registered().
		 * If not, look in the type-specific table.
		 *
		 * This happens when the first few bytes of the atom arena are shared
		 * by atoms of different types (it creates conflicts in the global
		 * ht_all_atoms table).
		 */

		if (g_hash_table_lookup_extended(td->table, key, &orig_key, &value)) {
			size = (size_t) value;
			g_assert(size >= ARENA_OFFSET);
		} else {
			size = 0;
		}
	}

	/*
	 * If atom exists, increment ref count and return it.
	 */

	if (size > 0) {
		atom_t *a;

		a = atom_from_arena(orig_key);
		g_assert(a->refcnt > 0);

		atom_unprotect(a, size);
		a->refcnt++;
		atom_protect(a, size);
		return orig_key;
	} else {
		size_t len;
		gpointer value;
		atom_t *a;

		/*
		 * Create new atom.
		 */

		len = (*td->len_func)(key);
		g_assert(len < ((size_t) -1) - ARENA_OFFSET);
		size = round_size_fast((ATOM_TYPE_MASK + 1), ARENA_OFFSET + len);

		a = atom_alloc(size);
		a->refcnt = 1;
		memcpy(atom_arena(a), key, len);
		atom_protect(a, size);

		/*
		 * Insert atom in table.
		 */

		value = (gpointer) (size | (guint) type);
		g_hash_table_insert(ht_all_atoms, atom_arena(a), value);
		g_hash_table_insert(td->table, atom_arena(a), (gpointer) size);

		return atom_arena(a);
	}
}

/**
 * Remove one reference from atom.
 * Dispose of atom if nobody references it anymore.
 */
void
atom_free(enum atom_type type, gconstpointer key)
{
	size_t size;
	atom_t *a;

    g_assert(key != NULL);
	g_assert(UNSIGNED(type) < G_N_ELEMENTS(atoms));

	size = atom_is_registered(type, key);
	g_assert(size > 0);

	a = atom_from_arena(key);
	g_assert(a->refcnt > 0);

	/*
	 * Dispose of atom when its reference count reaches 0.
	 */

	atom_unprotect(a, size);
	if (--a->refcnt == 0) {
		table_desc_t *td;

		td = &atoms[type];		/* Where atoms of this type are held */
		g_hash_table_remove(td->table, key);
		g_hash_table_remove(ht_all_atoms, key);
		atom_dealloc(a, size);
	} else {
		atom_protect(a, size);
	}
}

#ifdef TRACK_ATOMS

/** Information about given spot. */
struct spot {
	int count;			/**< Amount of allocation/free performed at spot */
};

/**
 * The tracking version of atom_get().
 *
 * @returns the atom's value.
 */
gconstpointer
atom_get_track(enum atom_type type, gconstpointer key, char *file, int line)
{
	gconstpointer atom;
	atom_t *a;
	char buf[512];
	gpointer k;
	gpointer v;
	struct spot *sp;

	atom = atom_get(type, key);
	a = atom_from_arena(atom);

	/*
	 * Initialize tracking tables on first allocation.
	 */

	if (a->refcnt == 1) {
		a->get = g_hash_table_new(g_str_hash, g_str_equal);
		a->free = g_hash_table_new(g_str_hash, g_str_equal);
	}

	gm_snprintf(buf, sizeof(buf), "%s:%d", short_filename(file), line);

	if (g_hash_table_lookup_extended(a->get, buf, &k, &v)) {
		sp = (struct spot *) v;
		sp->count++;
	} else {
		WALLOC(sp);
		sp->count = 1;
		g_hash_table_insert(a->get, g_strdup(buf), sp);
	}

	return atom;
}

/**
 * Free key/value pair of the tracking table.
 */
static gboolean
tracking_free_kv(gpointer key, gpointer value, gpointer uu_user)
{
	(void) uu_user;

	G_FREE_NULL(key);
	wfree(value, sizeof(struct spot));
	return TRUE;
}

/**
 * Get rid of the tracking hash table.
 */
static void
destroy_tracking_table(GHashTable *h)
{
	g_hash_table_foreach_remove(h, tracking_free_kv, NULL);
	g_hash_table_destroy(h);
}

/**
 * The tracking version of atom_free().
 * Remove one reference from atom.
 * Dispose of atom if nobody references it anymore.
 */
void
atom_free_track(enum atom_type type, gconstpointer key, char *file, int line)
{
	atom_t *a;
	char buf[512];
	gpointer k;
	gpointer v;
	struct spot *sp;

	a = atom_from_arena(key);

	/*
	 * If we're going to free the atom, dispose the tracking tables.
	 */

	if (a->refcnt == 1) {
		destroy_tracking_table(a->get);
		destroy_tracking_table(a->free);
	} else {
		gm_snprintf(buf, sizeof(buf), "%s:%d", short_filename(file), line);

		if (g_hash_table_lookup_extended(a->free, buf, &k, &v)) {
			sp = (struct spot *) v;
			sp->count++;
		} else {
			WALLOC(sp);
			sp->count = 1;
			g_hash_table_insert(a->free, g_strdup(buf), sp);
		}
	}

	atom_free(type, key);
}

/**
 * Dump all the spots where some tracked operation occurred, along with the
 * amount of such operations.
 */
static void
dump_tracking_entry(gpointer key, gpointer value, gpointer user)
{
	struct spot *sp = (struct spot *) value;
	const char *what = (const char *) user;

	g_warning("%10d %s at \"%s\"", sp->count, what, (char *) key);
}

/**
 * Dump the values held in the tracking table `h'.
 */
static void
dump_tracking_table(gpointer atom, GHashTable *h, char *what)
{
	guint count = g_hash_table_size(h);

	g_warning("all %u %s spot%s for 0x%lx:",
		count, what, count == 1 ? "" : "s", (gulong) atom);

	g_hash_table_foreach(h, dump_tracking_entry, what);
}

#endif	/* TRACK_ATOMS */

/**
 * Warning about existing atom that should have been freed.
 */
static gboolean
atom_warn_free(gpointer key, gpointer unused_value, gpointer udata)
{
	atom_t *a = atom_from_arena(key);
	table_desc_t *td = udata;

	(void) unused_value;

	g_warning("found remaining %s atom 0x%lx, refcnt=%d: \"%s\"",
		td->type, (glong) key, a->refcnt, (*td->str_func)(key));

#ifdef TRACK_ATOMS
	dump_tracking_table(key, a->get, "get");
	dump_tracking_table(key, a->free, "free");
	destroy_tracking_table(a->get);
	destroy_tracking_table(a->free);
#endif

	/*
	 * Don't free the entry, so that we know where the leak originates from
	 * when running under -DUSE_DMALLOC or via valgrind.
	 *		--RAM, 02/02/2003
	 */

	return TRUE;
}

/**
 * Shutdown atom structures, freeing all remaining atoms.
 */
void
atoms_close(void)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(atoms); i++) {
		table_desc_t *td = &atoms[i];

		g_hash_table_foreach_remove(td->table, atom_warn_free, td);
		gm_hash_table_destroy_null(&td->table);
	}
	gm_hash_table_destroy_null(&ht_all_atoms);
}

/* vi: set ts=4 sw=4 cindent: */
