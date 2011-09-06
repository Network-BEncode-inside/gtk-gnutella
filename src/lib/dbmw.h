/*
 * Copyright (c) 2008-2009, Raphael Manfredi
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
 * DBM wrapper for transparent serialization / deserialization
 * of data structures and cache management.
 *
 * @author Raphael Manfredi
 * @date 2008-2009
 */

#ifndef _dbmw_h_
#define _dbmw_h_

#include "dbmap.h"
#include "pmsg.h"
#include "bstr.h"

struct dbmw;
typedef struct dbmw dbmw_t;

/**
 * Serialization routine for values.
 *
 * @param mb		where the serialization will occur
 * @param data		data to serialize
 */
typedef void (*dbmw_serialize_t)(pmsg_t *mb, gconstpointer data);

/**
 * Deserialization routine for values.
 *
 * @param bs		where serialized value is held
 * @param valptr	where deserialization should be done
 * @param len		length of arena at valptr, for assertions
 *
 * It returns nothing: errors are diagnosed by looking at ``bs'' afterwards.
 */
typedef void (*dbmw_deserialize_t)(bstr_t *bs, gpointer valptr, size_t len);

/**
 * Free routine for values, to reclaim data allocated during deserialization
 * and which is referenced by the value, not to reclaim the memory used by the
 * value itself.
 *
 * @param valptr	where deserialization was done
 * @param len		length of arena allocated for value, for assertions
 */
typedef void (*dbmw_free_t)(gpointer valptr, size_t len);

/**
 * DBMW "foreach" iterator callbacks.
 *
 * @param key		pointer to constant-length key
 * @param value		deserialized value
 * @param len		length of arena where value was deserialized
 * @param u			user-supplied additional callback argument
 *
 * @return TRUE if key/value pair is to be removed (for the dbmw_cbr_t callbak).
 */
typedef void (*dbmw_cb_t)(gpointer key, gpointer value, size_t len, gpointer u);
typedef gboolean (*dbmw_cbr_t)(
	gpointer key, gpointer value, size_t len, gpointer u);

/**
 * Flags for dbmw_sync().
 */
#define DBMW_SYNC_CACHE		(1 << 0)	/**< Sync DBMW local cache */
#define DBMW_SYNC_MAP		(1 << 1)	/**< Sync DBMW underlying map */
#define DBMW_DELETED_ONLY	(1 << 2)	/**< Only sync deleted keys */

dbmw_t *dbmw_create(dbmap_t *dm, const char *name,
	size_t value_size, size_t value_data_size,
	dbmw_serialize_t pack, dbmw_deserialize_t unpack, dbmw_free_t valfree,
	size_t cache_size, GHashFunc hash_func, GEqualFunc eq_func);
void dbmw_destroy(dbmw_t *dw, gboolean close_sdbm);
ssize_t dbmw_sync(dbmw_t *dw, int which);
void dbmw_write(dbmw_t *dw, gconstpointer key, gpointer value, size_t length);
void dbmw_write_nocache(
	dbmw_t *dw, gconstpointer key, gpointer value, size_t length);
gpointer dbmw_read(dbmw_t *dw, gconstpointer key, size_t *lenptr);
gboolean dbmw_exists(dbmw_t *dw, gconstpointer key);
void dbmw_delete(dbmw_t *dw, gconstpointer key);
enum dbmap_type dbmw_map_type(const dbmw_t *dw);
size_t dbmw_count(dbmw_t *dw);
gboolean dbmw_has_ioerr(const dbmw_t *dw);
const char *dbmw_name(const dbmw_t *dw);
gboolean dbmw_set_map_cache(dbmw_t *dw, long pages);
gboolean dbmw_set_volatile(dbmw_t *dw, gboolean is_volatile);
gboolean dbmw_shrink(dbmw_t *dw);
gboolean dbmw_clear(dbmw_t *dw);
const char *dbmw_strerror(const dbmw_t *dw);

GSList *dbmw_all_keys(dbmw_t *dw);
void dbmw_free_all_keys(const dbmw_t *dw, GSList *keys);

void dbmw_foreach(dbmw_t *dw, dbmw_cb_t cb, gpointer arg);
void dbmw_foreach_remove(dbmw_t *dw, dbmw_cbr_t cbr, gpointer arg);

gboolean dbmw_store(dbmw_t *dw, const char *base, gboolean inplace);
gboolean dbmw_copy(dbmw_t *from, dbmw_t *to);

#endif /* _dbmw_h_ */

/* vi: set ts=4 sw=4 cindent: */
