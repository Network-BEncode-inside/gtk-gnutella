/*
 * $Id$
 *
 * Copyright (c) 2008, Raphael Manfredi
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
 * DB map generic interface.
 *
 * Keys need to be of constant width for this interface to be able to
 * mimic that of an in-core map.
 *
 * The purpose of the DB map is to offer a polymorphic implementation of
 * a map-like structure that can also be stored to disk in a DBM-like
 * hash-to-disk database.  That way, we can add more DBM-like backends
 * without having the change the client code.
 *
 * Another advantage is that we can provide easily a transparent fallback
 * to an in-core version of a DBM database should there be a problem with
 * initialization of the DBM.
 *
 * @author Raphael Manfredi
 * @date 2008
 */

#include "common.h"

RCSID("$Id$")

#include "sdbm/sdbm.h"

#include "dbmap.h"
#include "atoms.h"
#include "bstr.h"
#include "debug.h"
#include "map.h"
#include "pmsg.h"
#include "walloc.h"
#include "override.h"			/* Must be the last header included */

enum dbmap_magic { DBMAP_MAGIC = 0x5890dc4fU };

/**
 * The map structure holding the necessary information to delegate all
 * the operations to different implementations.
 */
struct dbmap {
	enum dbmap_magic magic;
	enum dbmap_type type;
	union {
		struct {
			map_t *map;
		} m;
		struct {
			DBM *sdbm;
			const char *path;		/**< SDBM file path (atom), if known */
			gboolean is_volatile;	/**< Whether DB can be discarded */
		} s;
	} u;
	size_t key_size;		/**< Constant width keys are a requirement */
	size_t count;			/**< Amount of items */
	int error;				/**< Last errno value consecutive to an error */
	unsigned ioerr:1;		/**< Last operation raised an I/O error */
	unsigned had_ioerr:1;	/**< Whether we ever had an I/O error */
	unsigned validated:1;	/**< Whether we initiated an initial keychek */
};

static inline void
dbmap_check(const dbmap_t *dm)
{
	g_assert(dm != NULL);
	g_assert(DBMAP_MAGIC == dm->magic);
}

/**
 * Special key used by dbmap_store() and used by dbmap_retrieve() to
 * persist informations necessary to reconstruct a DB map object easily.
 */
static const char dbmap_superkey[] = "__dbmap_superkey__";

#define DBMAP_SUPERKEY_VERSION	1U

/**
 * Superblock stored in the superkey.
 */
struct dbmap_superblock {
	guint32 key_size;		/**< Constant width keys are a requirement */
	guint32 count;			/**< Amount of items */
	guint32 flags;			/**< Status flags */
};

/**
 * Superblock status flags.
 */
#define DBMAP_SF_KEYCHECK (1 << 0)	/**< Need keycheck at next startup */

/**
 * Store a superblock in an SDBM DB map.
 * @return TRUE on success.
 */
static gboolean
dbmap_sdbm_store_superblock(const dbmap_t *dm)
{
	datum key, value;
	DBM *sdbm;
	pmsg_t *mb;
	guint32 flags = 0;
	gboolean ok = TRUE;

	dbmap_check(dm);
	g_assert(DBMAP_SDBM == dm->type);

	sdbm = dm->u.s.sdbm;

	key.dptr = deconstify_gpointer(dbmap_superkey);
	key.dsize = CONST_STRLEN(dbmap_superkey);

	if (dm->had_ioerr) {
		flags |= DBMAP_SF_KEYCHECK;		/* Request check next time */
	}

	/*
	 * Superblock stored in the superkey.
	 */

	mb = pmsg_new(PMSG_P_DATA, NULL, 3 * 4 + 1);
	pmsg_write_u8(mb, DBMAP_SUPERKEY_VERSION);
	pmsg_write_be32(mb, dm->key_size);
	pmsg_write_be32(mb, dm->count);
	pmsg_write_be32(mb, flags);

	value.dptr = pmsg_start(mb);
	value.dsize = pmsg_size(mb);

	if (-1 == sdbm_store(sdbm, key, value, DBM_REPLACE)) {
		ok = FALSE;
	}

	pmsg_free(mb);
	return ok;
}

/**
 * Read the superblock stored in an opened SDBM file.
 * @return TRUE if we read the superblock correctly.
 */
static gboolean
dbmap_sdbm_retrieve_superblock(DBM *sdbm, struct dbmap_superblock *block)
{
	datum key, value;
	gboolean ok;
	bstr_t *bs;
	guint8 version;

	key.dptr = deconstify_gpointer(dbmap_superkey);
	key.dsize = CONST_STRLEN(dbmap_superkey);

	value = sdbm_fetch(sdbm, key);

	if (NULL == value.dptr)
		return FALSE;

	bs = bstr_open(value.dptr, value.dsize, 0);

	if (value.dsize > 2 * 4) {
		bstr_read_u8(bs, &version);
	} else {
		version = 0;
	}

	if (version > DBMAP_SUPERKEY_VERSION) {
		g_warning("SDBM \"%s\": superblock more recent "
			"(version %u, can only understand up to version %u)",
			sdbm_name(sdbm), version, DBMAP_SUPERKEY_VERSION);
	}

	bstr_read_be32(bs, &block->key_size);
	bstr_read_be32(bs, &block->count);

	if (version >= 1) {
		bstr_read_be32(bs, &block->flags);
	}

	ok = !bstr_has_error(bs);
	bstr_free(&bs);

	return ok;
}

/**
 * Remove superblock from the SDBM file.
 * @return TRUE on success.
 */
static gboolean
dbmap_sdbm_strip_superblock(DBM *sdbm)
{
	datum key;

	g_assert(!sdbm_rdonly(sdbm));

	key.dptr = deconstify_gpointer(dbmap_superkey);
	key.dsize = CONST_STRLEN(dbmap_superkey);
	
	if (0 == sdbm_delete(sdbm, key))
		return TRUE;

	g_warning("SDBM \"%s\": cannot strip superblock: %s",
		sdbm_name(sdbm), g_strerror(errno));

	return FALSE;
}

/**
 * Check whether last operation reported an I/O error in the SDBM layer.
 *
 * If database is volatile, clear the error indication to continue
 * processing: the DB may end-up being corrupted of course, but upper layers
 * must be robust enough to cope with that fact.
 *
 * @return TRUE on error
 */
static gboolean
dbmap_sdbm_error_check(const dbmap_t *dm)
{
	dbmap_check(dm);
	g_assert(DBMAP_SDBM == dm->type);

	if (sdbm_error(dm->u.s.sdbm)) {
		dbmap_t *dmw = deconstify_gpointer(dm);
		dmw->ioerr = TRUE;
		dmw->had_ioerr = TRUE;
		dmw->error = errno;
		if (dm->u.s.is_volatile) {
			sdbm_clearerr(dm->u.s.sdbm);
		}
		return TRUE;
	} else if (dm->ioerr) {
		dbmap_t *dmw = deconstify_gpointer(dm);
		dmw->ioerr = FALSE;
		dmw->error = 0;
	}

	return FALSE;
}

/**
 * Helper routine to count keys in an opened SDBM database.
 */
size_t
dbmap_sdbm_count_keys(dbmap_t *dm, gboolean expect_superblock)
{
	datum key;
	size_t count = 0;
	struct dbmap_superblock sblock;
	DBM* sdbm;

	dbmap_check(dm);
	g_assert(DBMAP_SDBM == dm->type);

	sdbm = dm->u.s.sdbm;

	/*
	 * If there is a superblock, use it to read key count, then strip it.
	 */

	if (dbmap_sdbm_retrieve_superblock(sdbm, &sblock)) {
		if (common_dbg) {
			g_debug("SDBM \"%s\": superblock has %u key%s%s",
				sdbm_name(sdbm), (unsigned) sblock.count,
				1 == sblock.count ? "" : "s",
				(sblock.flags & DBMAP_SF_KEYCHECK) ?
					" (keycheck required)" : "");
		}

		dbmap_sdbm_strip_superblock(sdbm);
		if (expect_superblock && !(sblock.flags & DBMAP_SF_KEYCHECK))
			return sblock.count;
	} else if (expect_superblock) {
		if (common_dbg) {
			g_debug("SDBM \"%s\": no superblock, counting and checking keys",
				sdbm_name(sdbm));
		}
	}

	for (key = sdbm_firstkey_safe(sdbm); key.dptr; key = sdbm_nextkey(sdbm))
		count++;

	dm->validated = TRUE;

	if (sdbm_error(sdbm)) {
		g_warning("SDBM \"%s\": I/O error after key counting, clearing",
			sdbm_name(sdbm));
		sdbm_clearerr(sdbm);
	}

	return count;
}

/**
 * @return constant-width key size for the DB map.
 */
size_t
dbmap_key_size(const dbmap_t *dm)
{
	return dm->key_size;
}

/**
 * Check whether I/O error has occurred.
 */
gboolean
dbmap_has_ioerr(const dbmap_t *dm)
{
	return dm->ioerr;
}

/**
 * Error string for last error.
 */
const char *
dbmap_strerror(const dbmap_t *dm)
{
	return g_strerror(dm->error);
}

/**
 * @return type of DB map.
 */
enum dbmap_type
dbmap_type(const dbmap_t *dm)
{
	return dm->type;
}

/**
 * @return amount of items held in map
 */
size_t
dbmap_count(const dbmap_t *dm)
{
	if (DBMAP_MAP == dm->type) {
		size_t count = map_count(dm->u.m.map);
		g_assert(dm->count == count);
	}

	return dm->count;
}

/**
 * Create a DB back-end implemented in memory as a hash table.
 *
 * @param key_size		expected constant key length
 * @param hash_func		the hash function for keys
 * @param key_eq_func	the key comparison function
 *
 * @return the new DB map
 */
dbmap_t *
dbmap_create_hash(size_t key_size, GHashFunc hash_func, GEqualFunc key_eq_func)
{
	dbmap_t *dm;

	g_assert(key_size != 0);

	dm = walloc0(sizeof *dm);
	dm->magic = DBMAP_MAGIC;
	dm->type = DBMAP_MAP;
	dm->key_size = key_size;
	dm->u.m.map = map_create_hash(hash_func, key_eq_func);

	return dm;
}

/**
 * Create a DB map implemented as a SDBM database.
 *
 * @param ksize		expected constant key length
 * @param name		name of the SDBM database, for logging (may be NULL)
 * @param path		path of the SDBM database
 * @param flags		opening flags
 * @param mode		file permissions
 *
 * @return the opened database, or NULL if an error occurred during opening.
 */
dbmap_t *
dbmap_create_sdbm(size_t ksize,
	const char *name, const char *path, int flags, int mode)
{
	dbmap_t *dm;

	g_assert(ksize != 0);
	g_assert(path);

	dm = walloc0(sizeof *dm);
	dm->type = DBMAP_SDBM;
	dm->key_size = ksize;
	dm->u.s.sdbm = sdbm_open(path, flags, mode);

	if (!dm->u.s.sdbm) {
		wfree(dm, sizeof *dm);
		return NULL;
	}

	dm->magic = DBMAP_MAGIC;
	dm->u.s.path = atom_str_get(path);

	if (name)
		sdbm_set_name(dm->u.s.sdbm, name);

	dm->count = dbmap_sdbm_count_keys(dm, !(flags & O_TRUNC));

	return dm;
}

/**
 * Create a map out of an existing map.
 * Use dbmap_release() to discard the dbmap encapsulation.
 *
 * @param key_size	expected constant key length of map
 * @param map		the already created map (may contain data)
 */
dbmap_t *
dbmap_create_from_map(size_t key_size, map_t *map)
{
	dbmap_t *dm;

	g_assert(key_size != 0);
	g_assert(map);

	dm = walloc0(sizeof *dm);
	dm->magic = DBMAP_MAGIC;
	dm->type = DBMAP_MAP;
	dm->key_size = key_size;
	dm->count = map_count(map);
	dm->u.m.map = map;

	return dm;
}

/**
 * Create a DB map out of an existing SDBM handle.
 * Use dbmap_release() to discard the dbmap encapsulation.
 *
 * @param name		name to give to the SDBM database (may be NULL)
 * @param key_size	expected constant key length of map
 * @param sdbm		the already created SDBM handle (DB may contain data)
 */
dbmap_t *
dbmap_create_from_sdbm(const char *name, size_t key_size, DBM *sdbm)
{
	dbmap_t *dm;

	g_assert(key_size != 0);
	g_assert(sdbm);

	if (name)
		sdbm_set_name(sdbm, name);

	dm = walloc0(sizeof *dm);
	dm->magic = DBMAP_MAGIC;
	dm->type = DBMAP_SDBM;
	dm->key_size = key_size;
	dm->u.s.sdbm = sdbm;
	dm->count = dbmap_sdbm_count_keys(dm, FALSE);

	return dm;
}

/**
 * Set the name of an underlying SDBM database.
 */
void
dbmap_sdbm_set_name(const dbmap_t *dm, const char *name)
{
	dbmap_check(dm);
	g_assert(name != NULL);
	g_assert(DBMAP_SDBM == dm->type);

	sdbm_set_name(dm->u.s.sdbm, name);
}

/**
 * Insert a key/value pair in the DB map.
 *
 * @return success status.
 */
gboolean
dbmap_insert(dbmap_t *dm, gconstpointer key, dbmap_datum_t value)
{
	dbmap_check(dm);

	switch (dm->type) {
	case DBMAP_MAP:
		{
			dbmap_datum_t *d = walloc(sizeof *d);
			gpointer okey;
			gpointer ovalue;
			gboolean found;

			if (value.data != NULL) {
				d->data = wcopy(value.data, value.len);
				d->len = value.len;
			} else {
				d->data = NULL;
				d->len = 0;
			}
		
			found = map_lookup_extended(dm->u.m.map, key, &okey, &ovalue);
			if (found) {
				dbmap_datum_t *od = ovalue;

				g_assert(dm->count);
				map_replace(dm->u.m.map, okey, d);
				if (od->data != NULL)
					wfree(od->data, od->len);
				wfree(od, sizeof *od);
			} else {
				gpointer dkey = wcopy(key, dm->key_size);

				map_insert(dm->u.m.map, dkey, d);
				dm->count++;
			}
		}
		break;
	case DBMAP_SDBM:
		{
			datum dkey;
			datum dval;
			gboolean existed = FALSE;
			int ret;

			dkey.dptr = deconstify_gpointer(key);
			dkey.dsize = dm->key_size;
			dval.dptr = deconstify_gpointer(value.data);
			dval.dsize = value.len;

			errno = dm->error = 0;
			ret = sdbm_replace(dm->u.s.sdbm, dkey, dval, &existed);
			if (0 != ret) {
				dbmap_sdbm_error_check(dm);
				return FALSE;
			}
			if (!existed)
				dm->count++;
		}
		break;
	case DBMAP_MAXTYPE:
		g_assert_not_reached();
	}

	return TRUE;
}

/**
 * Remove a key from the DB map.
 *
 * @return success status (not whether the key was present, rather whether
 * the key has been physically removed).
 */
gboolean
dbmap_remove(dbmap_t *dm, gconstpointer key)
{
	dbmap_check(dm);

	switch (dm->type) {
	case DBMAP_MAP:
		{
			gpointer dkey;
			gpointer dvalue;
			gboolean found;
		
			found = map_lookup_extended(dm->u.m.map, key, &dkey, &dvalue);
			if (found) {
				dbmap_datum_t *d;

				map_remove(dm->u.m.map, dkey);
				wfree(dkey, dm->key_size);
				d = dvalue;
				if (d->data != NULL)
					wfree(d->data, d->len);
				wfree(d, sizeof *d);
				g_assert(dm->count);
				dm->count--;
			}
		}
		break;
	case DBMAP_SDBM:
		{
			datum dkey;
			int ret;

			dkey.dptr = deconstify_gpointer(key);
			dkey.dsize = dm->key_size;

			errno = dm->error = 0;
			ret = sdbm_delete(dm->u.s.sdbm, dkey);
			dbmap_sdbm_error_check(dm);
			if (-1 == ret) {
				/* Could be that value was not found, errno == 0 then */
				if (errno != 0) {
					dm->error = errno;
					return FALSE;
				}
			} else {
				if (0 == dm->count) {
					if (dm->validated) {
						g_warning("DBMAP on sdbm \"%s\": BUG: "
							"sdbm_delete() worked but we had no key tracked",
							sdbm_name(dm->u.s.sdbm));
					} else {
						g_warning("DBMAP on sdbm \"%s\": "
							"key count inconsistency, validating database",
							sdbm_name(dm->u.s.sdbm));
					}
					dm->count = dbmap_sdbm_count_keys(dm, FALSE);
					g_warning("DBMAP on sdbm \"%s\": "
						"key count reset to %lu after counting",
						sdbm_name(dm->u.s.sdbm), (gulong) dm->count);
				} else {
					dm->count--;
				}
			}
		}
		break;
	case DBMAP_MAXTYPE:
		g_assert_not_reached();
	}

	return TRUE;
}

/**
 * Check whether DB map contains the key.
 */
gboolean
dbmap_contains(dbmap_t *dm, gconstpointer key)
{
	dbmap_check(dm);

	switch (dm->type) {
	case DBMAP_MAP:
		return map_contains(dm->u.m.map, key);
	case DBMAP_SDBM:
		{
			datum dkey;
			int ret;

			dkey.dptr = deconstify_gpointer(key);
			dkey.dsize = dm->key_size;

			dm->error = errno = 0;
			ret = sdbm_exists(dm->u.s.sdbm, dkey);
			dbmap_sdbm_error_check(dm);
			if (-1 == ret) {
				dm->error = errno;
				return FALSE;
			}
			return 0 != ret;
		}
	case DBMAP_MAXTYPE:
		g_assert_not_reached();
	}
	return FALSE;
}

/**
 * Lookup a key in the DB map.
 */
dbmap_datum_t
dbmap_lookup(dbmap_t *dm, gconstpointer key)
{
	dbmap_datum_t result = { NULL, 0 };

	dbmap_check(dm);

	switch (dm->type) {
	case DBMAP_MAP:
		{
			dbmap_datum_t *d;

			d = map_lookup(dm->u.m.map, key);
			if (d)
				result = *d;		/* struct copy */
			else {
				result.data = NULL;
				result.len = 0;
			}
		}
		break;
	case DBMAP_SDBM:
		{
			datum dkey;
			datum value;

			dkey.dptr = deconstify_gpointer(key);
			dkey.dsize = dm->key_size;

			errno = dm->error = 0;
			value = sdbm_fetch(dm->u.s.sdbm, dkey);
			dbmap_sdbm_error_check(dm);
			if (errno)
				dm->error = errno;
			result.data = value.dptr;
			result.len = value.dsize;
		}
		break;
	case DBMAP_MAXTYPE:
		g_assert_not_reached();
	}

	return result;
}

/**
 * Returns the underlying dbmap implementation.
 */
gpointer
dbmap_implementation(const dbmap_t *dm)
{
	dbmap_check(dm);

	switch (dm->type) {
	case DBMAP_MAP:
		return dm->u.m.map;
	case DBMAP_SDBM:
		return dm->u.s.sdbm;
	case DBMAP_MAXTYPE:
		g_assert_not_reached();
	}

	return NULL;
}

/**
 * Release the map encapsulation, returning the underlying implementation
 * object (will need to be cast back to the proper type for perusal).
 */
gpointer
dbmap_release(dbmap_t *dm)
{
	gpointer implementation;

	dbmap_check(dm);

	implementation = dbmap_implementation(dm);

	if (DBMAP_SDBM == dm->type) {
		atom_str_free_null(&dm->u.s.path);
	}

	dm->type = DBMAP_MAXTYPE;
	dm->magic = 0;
	wfree(dm, sizeof *dm);

	return implementation;
}

/**
 * Map iterator to free key/values
 */
static void
free_kv(gpointer key, gpointer value, gpointer u)
{
	dbmap_t *dm = u;
	dbmap_datum_t *d = value;

	wfree(key, dm->key_size);
	if (d->data != NULL)
		wfree(d->data, d->len);
	wfree(d, sizeof *d);
}

/**
 * Destroy a DB map.
 *
 * A memory-backed map is lost.
 * An SDBM-backed map is lost if marked volatile, provided its path is known.
 */
void
dbmap_destroy(dbmap_t *dm)
{
	dbmap_check(dm);

	switch (dm->type) {
	case DBMAP_MAP:
		map_foreach(dm->u.m.map, free_kv, dm);
		map_destroy(dm->u.m.map);
		break;
	case DBMAP_SDBM:
		sdbm_close(dm->u.s.sdbm);
		if (dm->u.s.is_volatile && dm->u.s.path != NULL)
			dbmap_unlink_sdbm(dm->u.s.path);
		atom_str_free_null(&dm->u.s.path);
		break;
	case DBMAP_MAXTYPE:
		g_assert_not_reached();
	}

	dm->type = DBMAP_MAXTYPE;
	dm->magic = 0;
	wfree(dm, sizeof *dm);
}

struct insert_ctx {
	GSList *sl;
	size_t key_size;
};

/**
 * Map iterator to insert a copy of the map keys into a singly-linked list.
 */
static void
insert_key(gpointer key, gpointer unused_value, gpointer u)
{
	gpointer kdup;
	struct insert_ctx *ctx = u;

	(void) unused_value;

	kdup = wcopy(key, ctx->key_size);
	ctx->sl = g_slist_prepend(ctx->sl, kdup);
}

/**
 * Snapshot all the constant-width keys, returning them in a singly linked list.
 * To free the returned keys, use the dbmap_free_all_keys() helper.
 */
GSList *
dbmap_all_keys(const dbmap_t *dm)
{
	GSList *sl = NULL;

	dbmap_check(dm);

	switch (dm->type) {
	case DBMAP_MAP:
		{
			struct insert_ctx ctx;

			ctx.sl = NULL;
			ctx.key_size = dm->key_size;
			map_foreach(dm->u.m.map, insert_key, &ctx);
			sl = ctx.sl;
		}
		break;
	case DBMAP_SDBM:
		{
			datum key;
			DBM *sdbm = dm->u.s.sdbm;

			errno = 0;
			for (
				key = sdbm_firstkey_safe(sdbm);
				key.dptr != NULL;
				key = sdbm_nextkey(sdbm)
			) {
				gpointer kdup;

				if (dm->key_size != key.dsize)
					continue;		/* Invalid key, corrupted file? */

				kdup = wcopy(key.dptr, key.dsize);
				sl = g_slist_prepend(sl, kdup);
			}
			dbmap_sdbm_error_check(dm);
		}
		break;
	case DBMAP_MAXTYPE:
		g_assert_not_reached();
	}

	return sl;
}

/**
 * Helper routine to free list and keys returned by dbmap_all_keys().
 */
void
dbmap_free_all_keys(const dbmap_t *dm, GSList *keys)
{
	GSList *sl;

	GM_SLIST_FOREACH(keys, sl) {
		wfree(sl->data, dm->key_size);
	}
	g_slist_free(keys);
}

/**
 * Structure used as context by dbmap_foreach_*trampoline().
 */
struct foreach_ctx {
	union {
		dbmap_cb_t cb;
		dbmap_cbr_t cbr;
	} u;
	gpointer arg;
};

/**
 * Trampoline to invoke the map iterator and do the proper casts.
 */
static void
dbmap_foreach_trampoline(gpointer key, gpointer value, gpointer arg)
{
	dbmap_datum_t *d = value;
	struct foreach_ctx *ctx = arg;

	(*ctx->u.cb)(key, d, ctx->arg);
}

/**
 * Trampoline to invoke the map iterator and do the proper casts.
 */
static gboolean
dbmap_foreach_remove_trampoline(gpointer key, gpointer value, gpointer arg)
{
	dbmap_datum_t *d = value;
	struct foreach_ctx *ctx = arg;
	gboolean to_remove;

	to_remove = (*ctx->u.cbr)(key, d, ctx->arg);

	if (to_remove) {
		if (d->data != NULL)
			wfree(d->data, d->len);
		wfree(d, sizeof *d);
	}

	return to_remove;
}

/**
 * Iterate over the map, invoking the callback on each item along with the
 * supplied argument.
 */
void
dbmap_foreach(const dbmap_t *dm, dbmap_cb_t cb, gpointer arg)
{
	dbmap_check(dm);
	g_assert(cb);

	switch (dm->type) {
	case DBMAP_MAP:
		{
			struct foreach_ctx ctx;

			ctx.u.cb = cb;
			ctx.arg = arg;
			map_foreach(dm->u.m.map, dbmap_foreach_trampoline, &ctx);
		}
		break;
	case DBMAP_SDBM:
		{
			datum key;
			DBM *sdbm = dm->u.s.sdbm;
			size_t count = 0;
			size_t invalid = 0;

			errno = 0;
			for (
				key = sdbm_firstkey(sdbm);
				key.dptr != NULL;
				key = sdbm_nextkey(sdbm)
			) {
				datum value;

				count++;

				if (dm->key_size != key.dsize) {
					invalid++;
					continue;		/* Invalid key, corrupted file? */
				}

				value = sdbm_value(sdbm);
				if (value.dptr) {
					dbmap_datum_t d;
					d.data = value.dptr;
					d.len = value.dsize;
					(*cb)(key.dptr, &d, arg);
				}
			}
			if (!dbmap_sdbm_error_check(dm)) {
				dbmap_t *dmw = deconstify_gpointer(dm);
				dmw->count = count;
			}
			if (invalid) {
				g_warning("DBMAP on sdbm \"%s\": found %lu invalid key%s",
					sdbm_name(sdbm), (gulong) invalid, 1 == invalid ? "" : "s");
			}
		}
		break;
	case DBMAP_MAXTYPE:
		g_assert_not_reached();
	}
}

/**
 * Iterate over the map, invoking the callback on each item along with the
 * supplied argument and removing the item when the callback returns TRUE.
 */
void
dbmap_foreach_remove(const dbmap_t *dm, dbmap_cbr_t cbr, gpointer arg)
{
	dbmap_check(dm);
	g_assert(cbr);

	switch (dm->type) {
	case DBMAP_MAP:
		{
			struct foreach_ctx ctx;
			dbmap_t *dmw;

			ctx.u.cbr = cbr;
			ctx.arg = arg;
			map_foreach_remove(dm->u.m.map,
				dbmap_foreach_remove_trampoline, &ctx);
			
			dmw = deconstify_gpointer(dm);
			dmw->count = map_count(dm->u.m.map);
		}
		break;
	case DBMAP_SDBM:
		{
			datum key;
			DBM *sdbm = dm->u.s.sdbm;
			size_t count = 0;
			size_t invalid = 0;

			errno = 0;
			for (
				key = sdbm_firstkey(sdbm);
				key.dptr != NULL;
				key = sdbm_nextkey(sdbm)
			) {
				datum value;

				count++;

				if (dm->key_size != key.dsize) {
					invalid++;
					continue;		/* Invalid key, corrupted file? */
				}

				value = sdbm_value(sdbm);
				if (value.dptr) {
					dbmap_datum_t d;
					d.data = value.dptr;
					d.len = value.dsize;
					if ((*cbr)(key.dptr, &d, arg)) {
						if (0 == sdbm_deletekey(sdbm)) {
							count--;
						}
					}
				}
			}
			dbmap_sdbm_error_check(dm);
			{
				dbmap_t *dmw = deconstify_gpointer(dm);
				dmw->count = count;
			}
			if (invalid) {
				g_warning("DBMAP on sdbm \"%s\": found %lu invalid key%s",
					sdbm_name(sdbm), (gulong) invalid, 1 == invalid ? "" : "s");
			}
		}
		break;
	case DBMAP_MAXTYPE:
		g_assert_not_reached();
	}
}

static void
unlink_sdbm(const char *file)
{
	if (-1 == unlink(file) && errno != ENOENT)
		g_warning("cannot unlink SDBM file %s: %s", file, g_strerror(errno));
}

/**
 * Helper routine to remove SDBM files under specified basename.
 */
void
dbmap_unlink_sdbm(const char *base)
{
	char *dir_file;
	char *pag_file;

	dir_file = g_strconcat(base, DBM_DIRFEXT, NULL);
	pag_file = g_strconcat(base, DBM_PAGFEXT, NULL);

	unlink_sdbm(dir_file);
	unlink_sdbm(pag_file);

	G_FREE_NULL(dir_file);
	G_FREE_NULL(pag_file);
}

static void
dbmap_store_entry(gpointer key, dbmap_datum_t *d, gpointer arg)
{
	dbmap_insert(arg, key, *d);
}

/**
 * Store DB map to disk in an SDBM database, at the specified base.
 * Two files are created (using suffixes .pag and .dir).
 *
 * If the map was already backed by an SDBM database and ``inplace'' is TRUE,
 * then the map is simply persisted as such.  It is marked non-volatile as
 * a side effect.
 *
 * @param dm		the DB map to store
 * @param base		base path for the persistent database
 * @param inplace	if TRUE and map was an SDBM already, persist as itself
 *
 * @return TRUE on success.
 */
gboolean
dbmap_store(dbmap_t *dm, const char *base, gboolean inplace)
{
	dbmap_t *ndm;
	gboolean ok = TRUE;

	dbmap_check(dm);

	if (inplace && DBMAP_SDBM == dm->type) {
		if (dbmap_sdbm_store_superblock(dm)) {
			dbmap_set_volatile(dm, FALSE);
			dbmap_sync(dm);
			return ok;
		}

		g_warning("SDBM \"%s\": cannot store superblock: %s",
			sdbm_name(dm->u.s.sdbm), g_strerror(errno));

		/* FALL THROUGH */
	}

	if (NULL == base)
		return FALSE;

	ndm = dbmap_create_sdbm(dm->key_size, NULL, base,
		O_CREAT | O_TRUNC | O_RDWR, S_IRUSR | S_IWUSR);

	if (!ndm) {
		g_warning("SDBM \"%s\": cannot store to %s: %s",
			sdbm_name(dm->u.s.sdbm), base, g_strerror(errno));
		return FALSE;
	}

	dbmap_foreach(dm, dbmap_store_entry, ndm);

	if (sdbm_error(ndm->u.s.sdbm)) {
		g_warning("SDBM \"%s\": cannot store to %s: errors during dump",
			sdbm_name(dm->u.s.sdbm), base);
		ok = FALSE;
		goto done;
	}

	dbmap_sdbm_store_superblock(ndm);

	/* FALL THROUGH */
done:
	dbmap_destroy(ndm);
	return ok;
}

/**
 * Copy context.
 */
struct copy_context {
	dbmap_t *to;			/**< Destination */
	gboolean error;			/**< Whether an error occurred */
};

static void
dbmap_copy_entry(gpointer key, dbmap_datum_t *d, gpointer arg)
{
	struct copy_context *ctx = arg;

	if (ctx->error)
		return;				/* Do not continue after an error */

	if (!dbmap_insert(ctx->to, key, *d))
		ctx->error = TRUE;
}

/**
 * Copy data from one DB map to another, replacing existing values (if the
 * destination was not empty and contained data for matching keys).
 *
 * @return TRUE on success.
 */
gboolean
dbmap_copy(dbmap_t *from, dbmap_t *to)
{
	struct copy_context ctx;

	dbmap_check(from);
	dbmap_check(to);

	if (from->key_size != to->key_size)
		return FALSE;

	ctx.to = to;
	ctx.error = FALSE;

	dbmap_foreach(from, dbmap_copy_entry, &ctx);

	return !ctx.error;
}

/**
 * Synchronize map.
 * @return amount of pages flushed to disk, or -1 in case of errors.
 */
ssize_t
dbmap_sync(dbmap_t *dm)
{
	dbmap_check(dm);

	switch (dm->type) {
	case DBMAP_MAP:
		return 0;
	case DBMAP_SDBM:
		return sdbm_sync(dm->u.s.sdbm);
	case DBMAP_MAXTYPE:
		g_assert_not_reached();
	}

	return 0;
}

/**
 * Attempt to shrink the database.
 * @return TRUE if no error occurred.
 */
gboolean
dbmap_shrink(dbmap_t *dm)
{
	dbmap_check(dm);

	switch (dm->type) {
	case DBMAP_MAP:
		return TRUE;
	case DBMAP_SDBM:
		return sdbm_shrink(dm->u.s.sdbm);
	case DBMAP_MAXTYPE:
		g_assert_not_reached();
	}

	return FALSE;
}

/**
 * Discard all data from the database.
 * @return TRUE if no error occurred.
 */
gboolean
dbmap_clear(dbmap_t *dm)
{
	dbmap_check(dm);

	switch (dm->type) {
	case DBMAP_MAP:
		map_foreach(dm->u.m.map, free_kv, dm);
		dm->count = 0;
		return TRUE;
	case DBMAP_SDBM:
		if (0 == sdbm_clear(dm->u.s.sdbm)) {
			dm->ioerr = FALSE;
			dm->count = 0;
			return TRUE;
		}
		return FALSE;
	case DBMAP_MAXTYPE:
		g_assert_not_reached();
	}

	return FALSE;
}

/**
 * Set SDBM cache size, in amount of pages (must be >= 1).
 * @return 0 if OK, -1 on errors with errno set.
 */
int
dbmap_set_cachesize(dbmap_t *dm, long pages)
{
	dbmap_check(dm);

	switch (dm->type) {
	case DBMAP_MAP:
		return 0;
	case DBMAP_SDBM:
		return sdbm_set_cache(dm->u.s.sdbm, pages);
	case DBMAP_MAXTYPE:
		g_assert_not_reached();
	}

	return 0;
}

/**
 * Turn SDBM deferred writes on or off.
 * @return 0 if OK, -1 on errors with errno set.
 */
int
dbmap_set_deferred_writes(dbmap_t *dm, gboolean on)
{
	dbmap_check(dm);

	switch (dm->type) {
	case DBMAP_MAP:
		return 0;
	case DBMAP_SDBM:
		return sdbm_set_wdelay(dm->u.s.sdbm, on);
	case DBMAP_MAXTYPE:
		g_assert_not_reached();
	}

	return 0;
}

/**
 * Tell SDBM whether it is volatile.
 * @return 0 if OK, -1 on errors with errno set.
 */
int
dbmap_set_volatile(dbmap_t *dm, gboolean is_volatile)
{
	dbmap_check(dm);

	switch (dm->type) {
	case DBMAP_MAP:
		return 0;
	case DBMAP_SDBM:
		dm->u.s.is_volatile = is_volatile;
		return sdbm_set_volatile(dm->u.s.sdbm, is_volatile);
	case DBMAP_MAXTYPE:
		g_assert_not_reached();
	}

	return 0;
}

/* vi: set ts=4 sw=4 cindent: */
