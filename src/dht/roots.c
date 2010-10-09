/*
 * $Id$
 *
 * Copyright (c) 2009, Raphael Manfredi
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
 * @ingroup dht
 * @file
 *
 * Lookup / publish root node caching.
 *
 * Whenever a value lookup or a publish is made, the set of k-closest nodes
 * surrounding the target KUID (including the nodes which returned the values
 * for value lookups) are cached.  Upon the next value lookup or publish for
 * the same KUID, the lookup shortlist will be seeded with these previous
 * root nodes, on the grounds that it is most likely that these nodes will be
 * still around.  This ensures faster convergence for lookups.
 *
 * The set of cached root nodes is kept for some time and is then discarded.
 * We may not need to publish / lookup the KUID, and too many stale seeds are
 * going to rather slow lookups down, since we'll have to wait for more RPC
 * timeouts before moving forward.
 *
 * The cache is organized as two separate DBMW databases + one table kept in
 * memory:
 *
 * + The memory table maps a target KUID to a structure keeping track of the
 *   last updates made to the root nodes for this KUID.
 *
 * + The first DBMW maps a KUID target to KDA_K dbkeys.  The intent is NOT to
 *   be able to share contact information between targets (this would involve
 *   refcounting and more I/O load for bookkeeping) but rather to prevent
 *   any size limitation of SDBM and keep values short-enough.
 *
 * + The second DBMW maps each dbkey to a node contact.
 *
 * @author Raphael Manfredi
 * @date 2009
 */

#include "common.h"

RCSID("$Id$")

#include "roots.h"
#include "keys.h"
#include "kuid.h"
#include "knode.h"
#include "storage.h"

#include "core/gnet_stats.h"

#include "if/gnet_property_priv.h"
#include "if/dht/kademlia.h"

#include "lib/atoms.h"
#include "lib/cq.h"
#include "lib/dbmap.h"
#include "lib/dbmw.h"
#include "lib/map.h"
#include "lib/patricia.h"
#include "lib/stringify.h"
#include "lib/tm.h"
#include "lib/unsigned.h"
#include "lib/vendors.h"
#include "lib/walloc.h"
#include "lib/override.h"		/* Must be the last header included */

#define ROOTS_CALLOUT		5000		/**< Heartbeat every 5 seconds */
#define ROOTKEY_LIFETIME	(2*3600*1000)	/**< 2 hours */
#define ROOTS_SYNC_PERIOD	60000		/**< Flush DB every minute */

#define ROOTKEYS_DB_CACHE_SIZE	512		/**< Cached amount of root keys */
#define CONTACT_DB_CACHE_SIZE	4096	/**< Cached amount of contacts */
#define CONTACT_MAP_CACHE_SIZE	128		/**< Amount of SDBM pages to cache */

/**
 * Private callout queue used to expire entries in the database that have
 * not been updated recently.
 */
static cqueue_t *roots_cq;

/**
 * Structure holding in-core information about all the target KUIDs we're
 * caching the roots for.
 *
 * We use a PATRICIA to be able to quickly locate roots close to a KUID.
 */
static patricia_t *roots;

/**
 * DBM wrapper to associate a target KUID with the set of KDA_K dbkeys.
 */
static dbmw_t *db_rootdata;
static char db_rootdata_base[] = "dht_roots";
static char db_rootdata_what[] = "DHT root node key datasets";

/**
 * DBM wrapper to associate a dbkey with the contact information.
 */
static dbmw_t *db_contact;
static char db_contact_base[] = "dht_root_contacts";
static char db_contact_what[] = "DHT root node contacts";

enum rootinfo_magic { ROOTINFO_MAGIC = 0x3320aefaU };

/**
 * Information about a target KUID that we're keeping in core.
 */
struct rootinfo {
	enum rootinfo_magic magic;
	kuid_t *kuid;				/**< The target key (atom) */
	cevent_t *expire_ev;		/**< The expire event for the key */
	time_t last_update;			/**< When we last updated the key set */
};

static inline void
rootinfo_check(const struct rootinfo *ri)
{
	g_assert(ri);
	g_assert(ROOTINFO_MAGIC == ri->magic);
}

#define ROOTDATA_STRUCT_VERSION	0

/**
 * Information about a target KUID that is stored to disk.
 * The structure is serialized first, not written as-is.
 */
struct rootdata {
	guint64 dbkeys[KDA_K];	/**< SDBM keys pointing to contact information */
	time_t last_update;		/**< When we last updated the key set */
	guint8 count;			/**< Amount of dbkeys contained */
	guint8 version;			/**< Structure version */
};

/**
 * Internal counter used to assign DB keys to the contacts we're storing.
 */
static guint64 contactid = 1;		/* 0 is not a valid key (used as marker) */

#define CONTACT_STRUCT_VERSION	0

/**
 * Contact information.
 * The structure is serialized first, not written as-is.
 */
struct contact {
	kuid_t *id;				/**< KUID of the node (atom) */
	vendor_code_t vcode;	/**< Vendor code */
	host_addr_t addr;		/**< IP of the node */
	guint16 port;			/**< Port of the node */
	guint8 major;			/**< Major version */
	guint8 minor;			/**< Minor version */
	guint8 version;			/**< Structure version */
};

static unsigned targets_managed;	/**< Amount of targets held in database */
static unsigned contacts_managed;	/**< Amount of contacts held in database */

/**
 * Allocate a new rootinfo structure.
 */
static struct rootinfo *
allocate_rootinfo(const kuid_t *kuid)
{
	struct rootinfo *ri;

	ri = walloc0(sizeof *ri);
	ri->magic = ROOTINFO_MAGIC;
	ri->kuid = kuid_get_atom(kuid);

	return ri;
}

/**
 * Discard rootinfo structure.
 */
static void
free_rootinfo(struct rootinfo *ri)
{
	rootinfo_check(ri);

	cq_cancel(roots_cq, &ri->expire_ev);
	kuid_atom_free_null(&ri->kuid);
	wfree(ri, sizeof *ri);
}

/**
 * Get rootdata from database.
 */
static struct rootdata *
get_rootdata(const kuid_t *id)
{
	struct rootdata *rd;

	rd = dbmw_read(db_rootdata, id, NULL);

	if (NULL == rd) {
		if (dbmw_has_ioerr(db_rootdata)) {
			g_warning("DBMW \"%s\" I/O error, bad things could happen...",
				dbmw_name(db_rootdata));
		} else {
			g_warning("key %s exists but was not found in DBMW \"%s\"",
				kuid_to_hex_string(id), dbmw_name(db_rootdata));
		}
	}

	return rd;
}

/**
 * Get contact from database.
 */
static struct contact *
get_contact(guint64 dbkey, gboolean shout)
{
	struct contact *c;

	c = dbmw_read(db_contact, &dbkey, NULL);

	if (NULL == c) {
		if (dbmw_has_ioerr(db_contact)) {
			g_warning("DBMW \"%s\" I/O error, bad things could happen...",
				dbmw_name(db_contact));
		} else if (shout) {
			g_warning("key %s exists but was not found in DBMW \"%s\"",
				uint64_to_string(dbkey), dbmw_name(db_contact));
		}
	}

	return c;
}

/**
 * Delete contact from database.
 */
static void
delete_contact(guint64 dbkey)
{
	g_assert(uint_is_positive(contacts_managed));

	contacts_managed--;
	gnet_stats_count_general(GNR_DHT_CACHED_ROOTS_HELD, -1);

	dbmw_delete(db_contact, &dbkey);

	if (GNET_PROPERTY(dht_roots_debug) > 2)
		g_message("DHT contact DB-key %s reclaimed", uint64_to_string(dbkey));
}

/**
 * Delete rootdata from database.
 */
static void
delete_rootdata(const kuid_t *id)
{
	const struct rootdata *rd;
	unsigned i;

	rd = get_rootdata(id);
	if (NULL == rd)
		return;

	for (i = 0; i < rd->count; i++) {
		delete_contact(rd->dbkeys[i]);
	}

	dbmw_delete(db_rootdata, id);

	if (GNET_PROPERTY(dht_roots_debug) > 2)
		g_message("DHT ROOTS k-closest nodes from %s reclaimed",
			kuid_to_hex_string(id));
}

/**
 * Map iterator callback to reclaim a DB-key value.
 */
static void
reclaim_dbkey(gpointer u_key, gpointer val, gpointer u_data)
{
	guint64 *dbkey = val;

	(void) u_key;
	(void) u_data;

	delete_contact(*dbkey);
}

/**
 * Callout queue callback to expire target.
 */
static void
roots_expire(cqueue_t *unused_cq, gpointer obj)
{
	struct rootinfo *ri = obj;
	(void) unused_cq;

	rootinfo_check(ri);
	g_assert(uint_is_positive(targets_managed));

	ri->expire_ev = NULL;		/* Event triggered */
	delete_rootdata(ri->kuid);
	patricia_remove(roots, ri->kuid);
	free_rootinfo(ri);
	targets_managed--;
	gnet_stats_count_general(GNR_DHT_CACHED_KUID_TARGETS_HELD, -1);
}

/**
 * Record k-closest roots we were able to locate around the specified KUID.
 *
 * @param nodes		the nodes that were successfully queried during lookup
 * @param kuid		the KUID target that was looked up
 */
void
roots_record(patricia_t *nodes, const kuid_t *kuid)
{
	struct rootinfo *ri;
	struct rootdata *rd;
	struct rootdata new_rd;
	struct {
		kuid_t id;
		guint64 dbkey;
	} previous[KDA_K];
	map_t *existing;
	patricia_iter_t *iter;
	unsigned i;
	unsigned new = 0, reused = 0;		/* For logging */
	gboolean existed = FALSE;			/* For logging */

	g_assert(nodes != NULL);
	g_assert(kuid != NULL);

	/*
	 * If KUID is within our k-ball, there's no need to cache the roots, as
	 * we routinely refresh our k-bucket and have normally a perfect knowledge
	 * of our KDA_K neighbours.
	 */

	if (keys_within_kball(kuid))
		return;

	ri = patricia_lookup(roots, kuid);

	if (NULL == ri) {
		ri = allocate_rootinfo(kuid);
		patricia_insert(roots, ri->kuid, ri);
		rd = &new_rd;
		new_rd.count = 0;
		targets_managed++;
		gnet_stats_count_general(GNR_DHT_CACHED_KUID_TARGETS_HELD, +1);
	} else {
		rd = get_rootdata(kuid);
		if (NULL == rd) {
			if (dbmw_has_ioerr(db_rootdata))
				return;			/* I/O error */
			/* Key supposed to exist but not found -> DB was corrupted */
			rd = &new_rd;
			new_rd.count = 0;
		} else {
			existed = TRUE;
		}
	}

	/*
	 * To avoid having to create new (and then delete old) contacts, we
	 * retrieve all the known old roots and map their KUID to the existing
	 * DB key that was used to retrieve each of them.  To prevent atom
	 * creation, we take a copy of the KUIDs and dbkeys on the stack.
	 *
	 * Only truly new contacts will be created, and only old contacts that
	 * are no longer present among the k-closest roots will be deleted.
	 */

	STATIC_ASSERT(G_N_ELEMENTS(rd->dbkeys) == G_N_ELEMENTS(previous));

	existing = map_create_patricia(KUID_RAW_BITSIZE);

	for (i = 0; i < rd->count; i++) {
		struct contact *c = get_contact(rd->dbkeys[i], FALSE);

		/*
		 * It can happen that ``rd'' contains DB keys that no longer exist
		 * in the contact database.  At startup time, we clear orphaned keys,
		 * but we do nothing against "ghost" DB keys referenced by entries
		 * within db_rootdata.  These are harmless anyway and will disappear
		 * as we expire entries or update them after a lookup.
		 *
		 * That's why the above get_contact() call silences "key not found"
		 * type of warnings.
		 */

		if (NULL == c)
			continue;		/* I/O error or stale info in ``rd'' */ 

		previous[i].id = *c->id;		/* Struct copy */
		previous[i].dbkey = rd->dbkeys[i];
		map_insert(existing, &previous[i].id, &previous[i].dbkey);
	}

	/*
	 * Now fetch the k-closest roots from the target KUID.
	 */

	iter = patricia_metric_iterator_lazy(nodes, kuid, TRUE);
	i = 0;

	while (patricia_iter_has_next(iter) && i < G_N_ELEMENTS(rd->dbkeys)) {
		knode_t *kn = patricia_iter_next_value(iter);
		guint64 *dbkey_ptr;

		/*
		 * If entry existed in the previous set, we reuse the old contact.
		 *
		 * Note that presence in the ``existing'' map means that we were
		 * able to read the contact from the database earlier, so this time
		 * we issue a verbose get_contact() call to warn if we fail to
		 * retrieve the contact from the database again.
		 */

		dbkey_ptr = map_lookup(existing, kn->id);

		if (NULL != dbkey_ptr) {
			struct contact *c = get_contact(*dbkey_ptr, TRUE);

			if (NULL == c)
				continue;		/* I/O error, most probably */

			/* Update contact addr:port information, if stale */
			if (c->port != kn->port || !host_addr_equal(c->addr, kn->addr)) {
				c->port = kn->port;
				c->addr = kn->addr;
				dbmw_write(db_contact, dbkey_ptr, c, sizeof *c);
				gnet_stats_count_general(
					GNR_DHT_CACHED_ROOTS_CONTACT_REFRESHED, 1);
			}

			map_remove(existing, kn->id);	/* We reused it */
			rd->dbkeys[i++] = *dbkey_ptr;	/* Reuse existing contact */
			reused++;
		} else {
			struct contact nc;
			guint64 dbkey;

			nc.id = kuid_get_atom(kn->id);	/* Freed through free_contact() */
			nc.vcode = kn->vcode;	/* Struct copy */
			nc.addr = kn->addr;		/* Struct copy */
			nc.port = kn->port;
			nc.major = kn->major;
			nc.minor = kn->minor;
			dbkey = contactid++;
			contacts_managed++;
			gnet_stats_count_general(GNR_DHT_CACHED_ROOTS_HELD, +1);

			dbmw_write(db_contact, &dbkey, &nc, sizeof nc);
				
			rd->dbkeys[i++] = dbkey;
			new++;
		}
	}

	rd->count = i;
	patricia_iterator_release(&iter);

	/*
	 * Any remaining entry in ``existing'' was not reused and can be deleted.
	 */

	map_foreach(existing, reclaim_dbkey, NULL);
	map_destroy(existing);

	/*
	 * Persist rootdata and update rootinfo.
	 */

	rd->last_update = tm_time();
	dbmw_write(db_rootdata, kuid, rd, sizeof *rd);

	if (ri->expire_ev) {
		cq_resched(roots_cq, ri->expire_ev, ROOTKEY_LIFETIME);
	} else {
		ri->expire_ev = cq_insert(roots_cq, ROOTKEY_LIFETIME, roots_expire, ri);
	}

	if (GNET_PROPERTY(dht_roots_debug) > 1) {
		g_message("DHT ROOTS cached %u/%u k-closest node%s to %s target %s "
			"(new=%u, reused=%u, elapsed=%s)",
			rd->count, (unsigned) patricia_count(nodes),
			1 == rd->count ? "" : "s",
			existed ? "existing" : "new",
			kuid_to_hex_string(kuid), new, reused,
			existed ?
				compact_time(delta_time(tm_time(), ri->last_update)) : "-");
	}

	ri->last_update = tm_time();
}

/*
 * Fill the supplied vector `kvec' whose size is `kcnt' with the knodes
 * that are the closest neighbours we have found.
 *
 * @param rd		the contact data we have found
 * @param kvec		base of the "knode_t *" vector
 * @param kcnt		size of the "knode_t *" vector
 * @param known		a PATRICIA containing known neighbours already.
 * @param furthest	if non-NULL, do not add nodes further away than this one
 * @param id		the target KUID, mandatory if furthest is non-NULL
 *
 * @return the amount of entries filled in the vector.  It will be up to the
 * caller to invoke knode_free() on the returned entries.
 */
static int
roots_fill_vector(struct rootdata *rd,
	knode_t **kvec, int kcnt, patricia_t *known,
	const knode_t *furthest, const kuid_t *id)
{
	int i;
	int j = 0;

	g_assert(NULL == furthest || id != NULL);

	for (i = 0; i < rd->count && i < kcnt; i++) {
		struct contact *c = get_contact(rd->dbkeys[i], FALSE);
		knode_t *kn;

		if (NULL == c)
			continue;		/* I/O error or corrupted database */

		if (patricia_contains(known, c->id))
			continue;

		/*
		 * If a furthest limit was given, skip nodes further away than
		 * that boundary.
		 */

		if (furthest != NULL && kuid_cmp3(id, c->id, furthest->id) >= 0)
			continue;

		kn = knode_new(c->id, 0,
			c->addr, c->port, c->vcode, c->major, c->minor);
		kn->flags |= KNODE_F_CACHED;
		kvec[j++] = kn;
	}

	return j;	/* Amount filled */
}

/**
 * Fill the supplied vector `kvec' whose size is `kcnt' with the knodes
 * that are the closest neighbours we know about from our cached roots.
 *
 * @param id		the KUID target they want neighbours for
 * @param kvec		base of the "knode_t *" vector
 * @param kcnt		size of the "knode_t *" vector
 * @param known		a PATRICIA containing known neighbours already.
 *
 * @return the amount of entries filled in the vector.  It will be up to the
 * caller to invoke knode_free() on the returned entries.
 */
int
roots_fill_closest(const kuid_t *id,
	knode_t **kvec, int kcnt, patricia_t *known)
{
	struct rootinfo *ri;
	int filled = 0;

	g_assert(id != NULL);
	g_assert(kcnt > 0);
	g_assert(kvec != NULL);
	g_assert(known != NULL);

	/*
	 * Do not count a cache miss if the lookup was for a key within
	 * our k-ball, since we do not cache roots for these keys.
	 */

	if (keys_within_kball(id)) {
		gnet_stats_count_general(GNR_DHT_CACHED_ROOTS_KBALL_LOOKUPS, 1);
		return 0;
	}

	ri = patricia_lookup(roots, id);

	if (NULL != ri) {
		struct rootdata *rd = get_rootdata(id);

		if (NULL == rd)
			return 0;			/* I/O error or corrupted database */

		/*
		 * We have an exact target match: return the nodes we have
		 * that are not already known.
		 */

		filled = roots_fill_vector(rd, kvec, kcnt, known, NULL, NULL);
		gnet_stats_count_general(GNR_DHT_CACHED_ROOTS_EXACT_HITS, 1);

		if (GNET_PROPERTY(dht_roots_debug) > 1) {
			g_message("DHT ROOTS exact match for %s (%s), filled %d new node%s",
				kuid_to_hex_string(id),
				compact_time(delta_time(tm_time(), ri->last_update)),
				filled, 1 == filled ? "" : "s");
		}
	} else {
		knode_t *furthest = patricia_furthest(known, id);
		struct rootinfo *cri = patricia_closest(roots, id);

		/*
		 * We don't have an exact target match but maybe we known about
		 * another key which is going to be close enough: if we find another
		 * ID which is closer to the target than the furthest known node so
		 * far, it may be worth it.
		 */

		if (
			cri != NULL &&
			(
				furthest == NULL ||			/* They have no known nodes */
				kuid_cmp3(id, cri->kuid, furthest->id) < 0
			)
		) {
			struct rootdata *rd = get_rootdata(cri->kuid);

			if (NULL == rd)
				return 0;		/* I/O error or corrupted database */

			filled = roots_fill_vector(rd, kvec, kcnt, known, furthest, id);

			if (filled > 0) {
				gnet_stats_count_general(
					GNR_DHT_CACHED_ROOTS_APPROXIMATE_HITS, 1);
			} else {
				gnet_stats_count_general(GNR_DHT_CACHED_ROOTS_MISSES, 1);
			}

			if (GNET_PROPERTY(dht_roots_debug) > 1) {
				g_message("DHT ROOTS approximate match of %s with %s (%s), "
					"filled %d new node%s",
					kuid_to_hex_string(id),
					kuid_to_hex_string2(cri->kuid),
					compact_time(delta_time(tm_time(), cri->last_update)),
					filled, 1 == filled ? "" : "s");
			}
		} else {
			gnet_stats_count_general(GNR_DHT_CACHED_ROOTS_MISSES, 1);

			if (GNET_PROPERTY(dht_roots_debug) > 1) {
				g_message("DHT ROOTS no suitable cached entry for %s, "
					"closest was %s",
					kuid_to_hex_string(id),
					cri != NULL ?
						kuid_to_hex_string2(cri->kuid) : "<none>");
			}
		}
	}

	return filled;
}

/**
 * Serialization routine for rootdata.
 */
static void
serialize_rootdata(pmsg_t *mb, gconstpointer data)
{
	const struct rootdata *rd = data;
	unsigned i;

	pmsg_write_u8(mb, rd->count);
	pmsg_write_time(mb, rd->last_update);
	for (i = 0; i < rd->count; i++) {
		pmsg_write_be64(mb, rd->dbkeys[i]);
	}

	/*
	 * Because this is persistent, version the structure so that changes
	 * can be processed efficiently after an upgrade.
	 *
	 * This is done here and not at the beginning of the serialized data
	 * because I forgot to plan for it before.
	 *		--RAM, 2009-10-18
	 */

	pmsg_write_u8(mb, ROOTDATA_STRUCT_VERSION);
}

/**
 * Deserialization routine for rootdata.
 */
static void
deserialize_rootdata(bstr_t *bs, gpointer valptr, size_t len)
{
	struct rootdata *rd = valptr;
	unsigned i;

	g_assert(sizeof *rd == len);

	bstr_read_u8(bs, &rd->count);
	bstr_read_time(bs, &rd->last_update);
	g_assert(rd->count <= G_N_ELEMENTS(rd->dbkeys));

	for (i = 0; i < rd->count; i++) {
		bstr_read_be64(bs, &rd->dbkeys[i]);
	}

	/*
	 * Temporary, until 0.96.7 is out: we cannot blindly read the version
	 * since it was lacking in previous experimental versions.  Therefore
	 * only do it if we have unread data.
	 *
	 * The test will be removed in versions after 0.96.7, when we can be
	 * certain that the new data format was serialized.
	 *		--RAM, 2009-10-18
	 */

	if (bstr_unread_size(bs))
		bstr_read_u8(bs, &rd->version);
	else
		rd->version = 0;
}

/**
 * Serialization routine for contacts.
 */
static void
serialize_contact(pmsg_t *mb, gconstpointer data)
{
	const struct contact *c = data;

	pmsg_write(mb, c->id->v, KUID_RAW_SIZE);
	pmsg_write_be32(mb, c->vcode.u32);
	pmsg_write_ipv4_or_ipv6_addr(mb, c->addr);
	pmsg_write_be16(mb, c->port);
	pmsg_write_u8(mb, c->major);
	pmsg_write_u8(mb, c->minor);

	/*
	 * Because this is persistent, version the structure so that changes
	 * can be processed efficiently after an upgrade.
	 *
	 * This is done here and not at the beginning of the serialized data
	 * because I forgot to plan for it before.
	 *		--RAM, 2009-10-18
	 */

	pmsg_write_u8(mb, CONTACT_STRUCT_VERSION);
}

/**
 * Deserialization routine for contacts.
 */
static void
deserialize_contact(bstr_t *bs, gpointer valptr, size_t len)
{
	struct contact *c = valptr;
	kuid_t id;

	g_assert(sizeof *c == len);

	c->id = NULL;

	bstr_read(bs, id.v, KUID_RAW_SIZE);
	bstr_read_be32(bs, &c->vcode.u32);
	bstr_read_packed_ipv4_or_ipv6_addr(bs, &c->addr);
	bstr_read_be16(bs, &c->port);
	bstr_read_u8(bs, &c->major);
	bstr_read_u8(bs, &c->minor);

	/*
	 * Temporary, until 0.96.7 is out: we cannot blindly read the version
	 * since it was lacking in previous experimental versions.  Therefore
	 * only do it if we have unread data.
	 *
	 * The test will be removed in versions after 0.96.7, when we can be
	 * certain that the new data format was serialized.
	 *		--RAM, 2009-10-18
	 */

	if (bstr_unread_size(bs))
		bstr_read_u8(bs, &c->version);
	else
		c->version = 0;

	/*
	 * Only create the KUID atom if there was no error in the deserialization
	 * since the value free callbacks are not invoked by the DBMW layer when
	 * there is a deserialization error.
	 */

	if (bstr_ended(bs)) {
		c->id = kuid_get_atom(&id);
	}
}

/**
 * Free routine for contacts, to release internally allocated memory, not
 * the structure itself.
 */
static void
free_contact(gpointer valptr, size_t len)
{
	struct contact *c = valptr;

	g_assert(sizeof *c == len);

	kuid_atom_free_null(&c->id);
}

/**
 * Context for recreate_ri() and remove_orphan().
 */
struct recreate_context {
	GHashTable *dbkeys;		/* Seen DB keys (atoms) */
	guint orphans;			/* Orphan keys found */
};

static void
free_dbkey_kv(gpointer key, gpointer u_value, gpointer u_data)
{
	guint64 *dbkey = key;

	(void) u_value;
	(void) u_data;

	atom_uint64_free(dbkey);
}

/**
 * DBMW foreach iterator to recreate keyinfo if not too ancient.
 * @return TRUE if entry is too ancient and key must be deleted.
 */
static gboolean
recreate_ri(gpointer key, gpointer value, size_t u_len, gpointer data)
{
	struct recreate_context *ctx = data;
	const struct rootdata *rd = value;
	const kuid_t *id = key;
	struct rootinfo *ri;
	time_delta_t d;
	int i;

	(void) u_len;

	/*
	 * If cached roots are too ancient, drop them.
	 */

	d = delta_time(tm_time(), rd->last_update);

	if (GNET_PROPERTY(dht_roots_debug) > 4)
		g_message("DHT ROOTS retrieved target %s (%s)",
			kuid_to_hex_string(id), compact_time(d));

	if (d >= ROOTKEY_LIFETIME / 1000) {
		for (i = 0; i < rd->count; i++) {
			dbmw_delete(db_contact, &rd->dbkeys[i]);
		}
		return TRUE;
	}

	/*
	 * OK, we can keep these roots.
	 *
	 * Be sure to remember the largest contact ID we keep, so that we can
	 * initialize our counter properly for creating new entries later on.
	 */

	for (i = 0; i < rd->count; i++) {
		guint64 dbkey = rd->dbkeys[i];

		if (dbkey >= contactid)
			contactid = dbkey + 1;

		if (!g_hash_table_lookup(ctx->dbkeys, &dbkey)) {
			const guint64 *dbatom = atom_uint64_get(&dbkey);
			gm_hash_table_insert_const(ctx->dbkeys, dbatom, uint_to_pointer(1));
		}
	}

	ri = allocate_rootinfo(id);
	patricia_insert(roots, ri->kuid, ri);
	ri->last_update = rd->last_update;
	ri->expire_ev = cq_insert(roots_cq, ROOTKEY_LIFETIME - d * 1000,
		roots_expire, ri);

	/*
	 * Update stats.
	 */

	targets_managed++;
	gnet_stats_count_general(GNR_DHT_CACHED_KUID_TARGETS_HELD, +1);

	contacts_managed += rd->count;
	gnet_stats_count_general(GNR_DHT_CACHED_ROOTS_HELD, rd->count);

	if (GNET_PROPERTY(dht_roots_debug) > 3)
		g_message("DHT ROOTS retrieved %u closest node%s from %s kept (for %s)",
			rd->count, 1 == rd->count ? "" : "s",
			kuid_to_hex_string(id), compact_time(ROOTKEY_LIFETIME / 1000 - d));

	return FALSE;
}

/**
 * DBMW foreach iterator to remove orphan DB keys.
 * @return TRUE if entry is an orphan and must be deleted.
 */
static gboolean
remove_orphan(gpointer key, gpointer u_value, size_t u_len, gpointer data)
{
	struct recreate_context *ctx = data;
	guint64 *dbkey = key;

	(void) u_value;
	(void) u_len;

	if (!g_hash_table_lookup(ctx->dbkeys, dbkey)) {
		ctx->orphans++;
		return TRUE;
	}

	return FALSE;
}

/**
 * Periodic DB synchronization.
 */
static gboolean
roots_sync(gpointer unused_obj)
{
	(void) unused_obj;

	storage_sync(db_rootdata);
	storage_sync(db_contact);

	return TRUE;
}

/**
 * Recreate rootinfo data from persisted information.
 */
static void
roots_init_rootinfo(void)
{
	struct recreate_context ctx;

	if (GNET_PROPERTY(dht_roots_debug)) {
		size_t count = dbmw_count(db_rootdata);
		g_message("DHT ROOTS scanning %u retrieved target KUID%s",
			(unsigned) count, 1 == count ? "" : "s");
	}

	ctx.dbkeys = g_hash_table_new(uint64_mem_hash, uint64_mem_eq);
	ctx.orphans = 0;

	dbmw_foreach_remove(db_rootdata, recreate_ri, &ctx);
	dbmw_foreach_remove(db_contact, remove_orphan, &ctx);

	g_hash_table_foreach(ctx.dbkeys, free_dbkey_kv, NULL);
	g_hash_table_destroy(ctx.dbkeys);

	if (GNET_PROPERTY(dht_roots_debug)) {
		size_t count = dbmw_count(db_rootdata);
		g_message("DHT ROOTS kept %u target KUID%s: targets=%u, contacts=%u",
			(unsigned) count, 1 == count ? "" : "s",
			targets_managed, contacts_managed);
		g_message("DHT ROOTS stripped %u orphan contact DB-key%s",
			ctx.orphans, 1 == ctx.orphans ? "" : "s");
		g_message("DHT ROOTS first allocated contact DB-key will be %s",
			uint64_to_string(contactid));
	}

	if (!dbmw_shrink(db_rootdata)) {
		if (GNET_PROPERTY(dht_roots_debug))
			g_warning("DHT ROOTS unable to shrink %s", db_rootdata_what);
	}

	if (!dbmw_shrink(db_contact)) {
		if (GNET_PROPERTY(dht_roots_debug))
			g_warning("DHT ROOTS unable to shrink %s", db_contact_what);
	}
}

/**
 * Initialize root node caching.
 */
void
roots_init(void)
{
	roots_cq = cq_submake("roots", callout_queue, ROOTS_CALLOUT);
	roots = patricia_create(KUID_RAW_BITSIZE);

	db_rootdata = storage_open(db_rootdata_what, db_rootdata_base,
		KUID_RAW_SIZE, sizeof(struct rootdata), 0,
		serialize_rootdata, deserialize_rootdata, NULL,
		ROOTKEYS_DB_CACHE_SIZE, sha1_hash, sha1_eq);

	db_contact = storage_open(db_contact_what, db_contact_base,
		sizeof(guint64), sizeof(struct contact),
			sizeof(struct contact) + KUID_RAW_SIZE,
		serialize_contact, deserialize_contact, free_contact,
		CONTACT_DB_CACHE_SIZE, uint64_mem_hash, uint64_mem_eq);

	dbmw_set_map_cache(db_contact, CONTACT_MAP_CACHE_SIZE);

	roots_init_rootinfo();
	cq_periodic_add(roots_cq, ROOTS_SYNC_PERIOD, roots_sync, NULL);
}

/**
 * Map iterator to free the items held in `roots'.
 */
static void
roots_free_kv(gpointer u_key, gpointer val, gpointer u_x)
{
	struct rootinfo *ri = val;

	(void) u_key;
	(void) u_x;

	free_rootinfo(ri);
}

/**
 * Close root node caching.
 */
void
roots_close(void)
{
	if (roots) {
		map_t *mr = map_create_from_patricia(roots);
		map_foreach(mr, roots_free_kv, NULL);
		map_release(mr);
		patricia_destroy(roots);
		roots = NULL;
	}

	if (GNET_PROPERTY(dht_roots_debug)) {
		g_message("DHT ROOTS shutdown with targets=%u, contacts=%u",
			targets_managed, contacts_managed);
	}

	storage_close(db_rootdata, db_rootdata_base);
	db_rootdata = NULL;

	storage_close(db_contact, db_contact_base);
	db_contact = NULL;

	cq_free_null(&roots_cq);
}

/* vi: set ts=4 sw=4 cindent: */
