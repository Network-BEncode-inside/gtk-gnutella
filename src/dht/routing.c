/*
 * $Id$
 *
 * Copyright (c) 2006-2009, Raphael Manfredi
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
 * Kademlia routing table.
 *
 * The Kademlia routing table is the central data structure governing all
 * the DHT operations pertaining to distribution (the 'D' of DHT).
 *
 * It is a specialized version of a trie, with leaves being the k-buckets.
 * Each leaf k-bucket contains contact information in the k-bucket, which is
 * stored in three lists:
 *
 *   the "good" list contains good contacts, with the newest at the tail.
 *   the "stale" list contains contacts for which an RPC timeout occurred.
 *   the "pending" list used to store contacts not added to a full "good" list
 *
 * The non-leaf trie nodes do not contain any information but simply serve
 * to connect the structure.
 *
 * The particularity of this trie is that we do not create children nodes
 * until a k-bucket is full, and we only split k-bucket to some maximal
 * depth.  The k-bucket which contains this Kademlia node's KUID is fully
 * splitable up to the maximum depth, and so is the tree closest to this
 * KUID, as defined in the is_splitable() routine.
 *
 * @author Raphael Manfredi
 * @date 2006-2009
 */

#include "common.h"

RCSID("$Id$")

#include <math.h>

#include "routing.h"
#include "acct.h"
#include "kuid.h"
#include "knode.h"
#include "rpc.h"
#include "lookup.h"
#include "token.h"
#include "keys.h"
#include "ulq.h"
#include "kmsg.h"
#include "publish.h"
#include "roots.h"
#include "tcache.h"
#include "stable.h"

#include "core/settings.h"
#include "core/gnet_stats.h"
#include "core/guid.h"
#include "core/nodes.h"
#include "core/sockets.h"

#include "if/gnet_property.h"
#include "if/gnet_property_priv.h"

#include "if/dht/routing.h"
#include "if/dht/dht.h"

#include "lib/atoms.h"
#include "lib/base16.h"
#include "lib/bit_array.h"
#include "lib/cq.h"
#include "lib/file.h"
#include "lib/getdate.h"
#include "lib/hashlist.h"
#include "lib/host_addr.h"
#include "lib/map.h"
#include "lib/patricia.h"
#include "lib/stats.h"
#include "lib/stringify.h"
#include "lib/timestamp.h"
#include "lib/vendors.h"
#include "lib/walloc.h"
#include "lib/override.h"		/* Must be the last header included */

#define K_BUCKET_GOOD		KDA_K	/* Keep k good contacts per k-bucket */
#define K_BUCKET_STALE		KDA_K	/* Keep k possibly "stale" contacts */
#define K_BUCKET_PENDING	KDA_K	/* Keep k pending contacts (replacement) */

#define K_BUCKET_MAX_DEPTH	(KUID_RAW_BITSIZE - 1)
#define K_BUCKET_MAX_DEPTH_PASSIVE	16

/**
 * How many sub-divisions of a bucket can happen.
 *
 * If set to 1, this is the normal basic Kademlia routing with each step
 * decreasing the distance by a factor 2.
 *
 * If set to b, with b > 1, then each lookup step will decrease the distance
 * by 2^b, but the k-buckets not containing our node ID will be further
 * subdivided by b-1 levels, thereby increase the size of the routing table
 * but buying us a more rapid convergence in remote ID spaces.
 */
#define K_BUCKET_SUBDIVIDE	(KDA_B)	/* Faster convergence => larger table */

/**
 * Maximum number of nodes from a class C network that can be in a k-bucket.
 * This is a way to fight against ID attacks from a hostile network: we
 * stop inserting hosts from that over-present network.
 */
#define K_BUCKET_MAX_IN_NET	3		/* At most 3 hosts from same class C net */

#define C_MASK	0xffffff00		/* Class C network mask */

/**
 * Period for aliveness checks.
 *
 * Every period, we make sure our "good" contacts are still alive and
 * check whether the "stale" contacts can be permanently dropped.
 */
#define ALIVE_PERIOD			(10*60)		/* 10 minutes */
#define ALIVE_PERIOD_MS			(ALIVE_PERIOD * 1000)
#define ALIVE_PERIOD_PASV		(20*60)		/* 20 minutes */
#define ALIVE_PERIOD_PASV_MS	(ALIVE_PERIOD_PASV * 1000)

/**
 * Period for bucket refreshes.
 *
 * Every period, a random ID falling in the bucket is generated and a
 * lookup is launched for that ID.
 */
#define REFRESH_PERIOD			(60*60)		/* 1 hour */
#define OUR_REFRESH_PERIOD		(15*60)		/* 15 minutes */

/*
 * K-bucket node information, accessed through the "kbucket" structure.
 */
struct kbnodes {
	hash_list_t *good;			/**< The good nodes */
	hash_list_t *stale;			/**< The (possibly) stale nodes */
	hash_list_t *pending;		/**< The nodes which are awaiting decision */
	GHashTable *all;			/**< All nodes in one of the lists */
	GHashTable *c_class;		/**< Counts class-C networks in bucket */
	cevent_t *aliveness;		/**< Periodic aliveness checks */
	cevent_t *refresh;			/**< Periodic bucket refresh */
	time_t last_lookup;			/**< Last time node lookup was performed */
};

/**
 * The routing table is a binary tree.  Each node holds a k-bucket containing
 * the contacts whose KUID falls within the range of the k-bucket.
 * Only leaf k-buckets contain nodes, the others are just holding the tree
 * structure together.
 */
struct kbucket {
	kuid_t prefix;				/**< Node prefix of the k-bucket */
	struct kbucket *parent;		/**< Parent node in the tree */
	struct kbucket *zero;		/**< Child node for "0" prefix */
	struct kbucket *one;		/**< Child node for "1" prefix */
	struct kbnodes *nodes;		/**< Node information, in leaf k-buckets */
	guchar depth;				/**< Depth in tree (meaningful bits) */
	guchar split_depth;			/**< Depth at which we left our space */
	gboolean ours;				/**< Whether our KUID falls in that bucket */
};

/**
 * A (locallay determined) size estimate.
 */
struct ksize {
	guint64 estimate;			/**< Value (64 bits should be enough!) */
	size_t amount;				/**< Amount of nodes used to compute estimate */
	time_t computed;			/**< When did we compute it? */
};

/**
 * A (network-received) remote size estimate
 */
struct nsize {
	time_t updated;				/**< When did we last update it? */
	hash_list_t *others;		/**< K_OTHER_SIZE items at most */
};

#define K_OTHER_SIZE		8	/**< Keep 8 network size estimates per region */
#define K_REGIONS			256	/**< Extra local size estimates after lookups */

#define K_LOCAL_ESTIMATE	(5 * KDA_K)		/**< # of nodes for local size */
#define MIN_ESTIMATE_NODES	15				/**< At least 15 data points */
#define ESTIMATE_LIFE		REFRESH_PERIOD	/**< Life of subspace estimates */

/**
 * Statistics on the routing table.
 */
struct kstats {
	int buckets;				/**< Total number of buckets */
	int leaves;					/**< Number of leaf buckets */
	int good;					/**< Number of good nodes */
	int stale;					/**< Number of stale nodes */
	int pending;				/**< Number of pending nodes */
	int max_depth;				/**< Maximum tree depth */
	struct ksize local;			/**< Local estimate based our neighbours */
	struct ksize average;		/**< Cached average DHT size estimate */
	struct ksize lookups[K_REGIONS];	/**< Estimates derived from lookups */
	struct nsize network[K_REGIONS];	/**< K_OTHER_SIZE items at most */
	statx_t *lookdata;			/**< Statistics on lookups[] */
	statx_t *netdata;			/**< Statistics on network[] */
	gboolean dirty;				/**< The "good" list was changed */
};

/**
 * Items for the stats.network[] lists.
 */
struct other_size {
	kuid_t *id;					/**< Node who made the estimate (atom) */
	guint64 size;				/**< Its own size estimate */
};

static gboolean initialized;		/**< Whether dht_init() was called */
static gboolean bootstrapping;		/**< Whether we are bootstrapping */
static enum dht_bootsteps old_boot_status = DHT_BOOT_NONE;

static struct kbucket *root = NULL;	/**< The root of the routing table tree. */
static kuid_t *our_kuid;			/**< Our own KUID (atom) */
static struct kstats stats;			/**< Statistics on the routing table */

static const gchar dht_route_file[] = "dht_nodes";
static const gchar dht_route_what[] = "the DHT routing table";
static const kuid_t kuid_null;

static void bucket_alive_check(cqueue_t *cq, gpointer obj);
static void bucket_refresh(cqueue_t *cq, gpointer obj);
static void dht_route_retrieve(void);
static struct kbucket *dht_find_bucket(const kuid_t *id);

/*
 * Define DHT_ROUTING_DEBUG to enable more costly run-time assertions which
 * make all hash list insertions O(n), basically.
 */
#undef DHT_ROUTING_DEBUG

static const char * const boot_status_str[] = {
	"not bootstrapped yet",			/**< DHT_BOOT_NONE */
	"seeded with some hosts",		/**< DHT_BOOT_SEEDED */
	"looking for our KUID",			/**< DHT_BOOT_OWN */
	"completing bucket bootstrap",	/**< DHT_BOOT_COMPLETING */
	"completely bootstrapped",		/**< DHT_BOOT_COMPLETED */
	"shutdowning",					/**< DHT_BOOT_SHUTDOWN */
};

/**
 * Provide human-readable boot status.
 */
static const char *
boot_status_to_string(enum dht_bootsteps status)
{
	size_t i = status;

	STATIC_ASSERT(DHT_BOOT_MAX_VALUE == G_N_ELEMENTS(boot_status_str));

	if (i >= G_N_ELEMENTS(boot_status_str))
		return "invalid boot status";

	return boot_status_str[i];
}

/**
 * Give a textual representation of the DHT mode.
 */
const char *
dht_mode_to_string(dht_mode_t mode)
{
	switch (mode) {
	case DHT_MODE_INACTIVE:		return "inactive";
	case DHT_MODE_ACTIVE:		return "active";
	case DHT_MODE_PASSIVE:		return "passive";
	case DHT_MODE_PASSIVE_LEAF:	return "leaf";
	}

	return "unknown";
}

/**
 * Invoked when they change the configured DHT mode or when the UDP firewalled
 * indication changes.
 */
void
dht_configured_mode_changed(dht_mode_t mode)
{
	dht_mode_t new_mode = mode;

	switch (mode) {
	case DHT_MODE_INACTIVE:
	case DHT_MODE_PASSIVE:
	case DHT_MODE_PASSIVE_LEAF:
		break;
	case DHT_MODE_ACTIVE:
		if (GNET_PROPERTY(is_udp_firewalled))
			new_mode = DHT_MODE_PASSIVE;
		break;
	}

	gnet_prop_set_guint32_val(PROP_DHT_CURRENT_MODE, new_mode);
}

/**
 * Is DHT running in active mode?
 */
gboolean
dht_is_active(void)
{
	return GNET_PROPERTY(dht_current_mode) == DHT_MODE_ACTIVE;
}

/**
 * Is bucket a leaf?
 */
static gboolean
is_leaf(const struct kbucket *kb)
{
	g_assert(kb);

	return kb->nodes && NULL == kb->zero && NULL == kb->one;
}

/**
 * Get the sibling of a k-bucket.
 */
static inline struct kbucket *
sibling_of(const struct kbucket *kb)
{
	struct kbucket *parent = kb->parent;

	if (!parent)
		return deconstify_gpointer(kb);		/* Root is its own sibling */

	return (parent->one == kb) ? parent->zero : parent->one;
}

/**
 * Is the bucket under the tree spanned by the parent?
 */
static gboolean
is_under(const struct kbucket *kb, const struct kbucket *parent)
{
	if (parent->depth >= kb->depth)
		return FALSE;

	return kuid_match_nth(&kb->prefix, &parent->prefix, parent->depth);
}

/**
 * Is the bucket in our closest subtree?
 */
static gboolean
is_among_our_closest(const struct kbucket *kb)
{
	struct kbucket *kours;

	g_assert(kb);

	kours = dht_find_bucket(our_kuid);

	g_assert(kours);

	if (NULL == kours->parent) {
		g_assert(kours == root);	/* Must be the sole instance */
		g_assert(kb == root);
		g_assert(kb->ours);

		return TRUE;
	}

	g_assert(kours->parent);

	if (is_under(kb, kours->parent)) {
		struct kbucket *sibling;

		/*
		 * The bucket we're trying to split is under the same tree as the
		 * parent of the leaf that would hold our node.
		 */

		if (kb->depth == kours->depth)
			return TRUE;		/* This is the sibling of our bucket */

		/*
		 * See whether it is the bucket or its sibling that has a prefix
		 * which is closer to our KUID: we can split only the closest one.
		 */

		sibling = sibling_of(kb);

		switch (kuid_cmp3(our_kuid, &kb->prefix, &sibling->prefix)) {
		case -1:	/* kb is the closest to our KUID */
			return TRUE;
		case +1:	/* the sibling is the closest to our KUID */
			break;
		default:
			g_assert_not_reached();	/* Not possible, siblings are different */
		}
	}

	return FALSE;
}

/**
 * Is the k-bucket splitable?
 */
static gboolean
is_splitable(const struct kbucket *kb)
{
	unsigned max_depth;

	g_assert(is_leaf(kb));

	/*
	 * Limit the depth of the tree to K_BUCKET_MAX_DEPTH_PASSIVE for passive
	 * nodes since they don't need to maintain a full table.
	 */

	max_depth = dht_is_active() ?
		K_BUCKET_MAX_DEPTH : K_BUCKET_MAX_DEPTH_PASSIVE;

	if (kb->depth >= max_depth)
		return FALSE;		/* Reached the bottom of the tree */

	if (kb->ours)
		return TRUE;		/* We can always split our own bucket */

	/*
	 * A passive node does not store data and does not need to replicate it
	 * to its k-closest neighbours and does not answer RPC calls.  Hence
	 * the routing table is only maintained so that we get reasonable
	 * anchoring points to start our lookups.
	 *
	 * Thus limit the size of the routing table (there will also be less
	 * PINGs sent and less table maintenance overhead) by disabling extra
	 * bucket splits (i.e. acting as if KDA_B = 1) and not bothering with
	 * closest subtree irregular splits.
	 */

	if (!dht_is_active())
		return FALSE;		/* No more splits */

	/*
	 * We are an active node. Allow for KDA_B extra splits for buckets that
	 * have left our closest subtree.
	 */

	if (kb->depth + 1 - kb->split_depth < K_BUCKET_SUBDIVIDE)
		return TRUE;		/* Extra subdivision for faster convergence */

	/*
	 * Now the tricky part: that of the closest subtree surrounding our node.
	 * Since we want a perfect knowledge of all the nodes surrounding us,
	 * we shall split buckets that are not in our space but are "close" to us.
	 */

	return is_among_our_closest(kb);
}

/**
 * Is the DHT "bootstrapped"?
 */
gboolean
dht_bootstrapped(void)
{
	return DHT_BOOT_COMPLETED == GNET_PROPERTY(dht_boot_status);
}

/**
 * Is the DHT "seeded"?
 */
gboolean
dht_seeded(void)
{
	return root && !is_leaf(root);		/* We know more than "k" hosts */
}

/**
 * Compute the hash list storing nodes with a given status.
 */
static inline hash_list_t *
list_for(const struct kbucket *kb, knode_status_t status)
{
	g_assert(kb);
	g_assert(kb->nodes);

	switch (status) {
	case KNODE_GOOD:
		return kb->nodes->good;
	case KNODE_STALE:
		return kb->nodes->stale;
	case KNODE_PENDING:
		return kb->nodes->pending;
	case KNODE_UNKNOWN:
		g_error("invalid state passed to list_for()");
	}

	/* NOTREACHED */
	return NULL;
}

/**
 * Compute how many nodes the leaf k-bucket contains for the given status.
 */
static guint
list_count(const struct kbucket *kb, knode_status_t status)
{
	hash_list_t *hl;

	g_assert(kb);
	g_assert(is_leaf(kb));

	hl = list_for(kb, status);

	return hash_list_length(hl);
}

/**
 * Same as list_count() but returns 0 if the bucket is not a leaf.
 */
static guint
safe_list_count(const struct kbucket *kb, knode_status_t status)
{
	return is_leaf(kb) ? list_count(kb, status) : 0;
}

#if 0		/* UNUSED */
/**
 * Compute how mnay nodes are held with a given status under all the leaves
 * of the k-bucket.
 */
static guint
recursive_list_count(const struct kbucket *kb, knode_status_t status)
{
	if (kb->nodes)
		return list_count(kb, status);

	g_assert(kb->zero != NULL);
	g_assert(kb->one != NULL);

	return
		recursive_list_count(kb->zero, status) +
		recursive_list_count(kb->one, status);
}
#endif

/**
 * Maximum size allowed for the lists of a given status.
 */
static inline size_t
list_maxsize_for(knode_status_t status)
{
	switch (status) {
	case KNODE_GOOD:
		return K_BUCKET_GOOD;
	case KNODE_STALE:
		return K_BUCKET_STALE;
	case KNODE_PENDING:
		return K_BUCKET_PENDING;
	case KNODE_UNKNOWN:
		g_error("invalid state passed to list_maxsize_for()");
	}

	/* NOTREACHED */
	return 0;
}

/**
 * Update statistics for status change.
 */
static inline void
list_update_stats(knode_status_t status, int delta)
{
	switch (status) {
	case KNODE_GOOD:
		stats.good += delta;
		gnet_stats_count_general(GNR_DHT_ROUTING_GOOD_NODES, delta);
		if (delta)
			stats.dirty = TRUE;
		break;
	case KNODE_STALE:
		stats.stale += delta;
		gnet_stats_count_general(GNR_DHT_ROUTING_STALE_NODES, delta);
		break;
	case KNODE_PENDING:
		stats.pending += delta;
		gnet_stats_count_general(GNR_DHT_ROUTING_PENDING_NODES, delta);
		break;
	case KNODE_UNKNOWN:
		g_error("invalid state passed to list_update_stats()");
	}

	/* NOTREACHED */
}

#ifdef DHT_ROUTING_DEBUG
/**
 * Check bucket list consistency.
 */
static void
check_leaf_list_consistency(
	const struct kbucket *kb, hash_list_t *hl, knode_status_t status)
{
	GList *nodes;
	GList *l;
	guint count = 0;

	g_assert(kb->nodes);
	g_assert(list_for(kb, status) == hl);

	nodes = hash_list_list(hl);

	for (l = nodes; l; l = g_list_next(l)) {
		knode_t *kn = l->data;

		knode_check(kn);
		g_assert(kn->status == status);
		count++;
	}

	g_assert(count == hash_list_length(hl));

	g_list_free(nodes);
}
#else
#define check_leaf_list_consistency(a, b, c)
#endif	/* DHT_ROUTING_DEBUG */

/**
 * Get our KUID.
 */
kuid_t *
get_our_kuid(void)
{
	return our_kuid;
}

/**
 * Get our Kademlia node, with an IPv4 listening address.
 */
knode_t *
get_our_knode(void)
{
	vendor_code_t gtkg;

	gtkg.u32 = T_GTKG;

	return knode_new(our_kuid,
		dht_is_active() ? 0 : KDA_MSG_F_FIREWALLED,
		listen_addr(), socket_listen_port(), gtkg,
		KDA_VERSION_MAJOR, KDA_VERSION_MINOR);
}

/*
 * Hash and equals functions for other_size items.
 *
 * The aim is to keep only one size estimate per remote ID: its latest one.
 * So we only hash/compare on the id of the data.
 */

static unsigned int
other_size_hash(gconstpointer key)
{
	const struct other_size *os = key;

	return sha1_hash(os->id);
}

static int
other_size_eq(gconstpointer a, gconstpointer b)
{
	const struct other_size *os1 = a;
	const struct other_size *os2 = b;

	return os1->id == os2->id;		/* Known to be atoms */
}

static void
other_size_free(struct other_size *os)
{
	g_assert(os);

	kuid_atom_free_null(&os->id);
	wfree(os, sizeof *os);
}

/**
 * Short description of a k-bucket for logs.
 * @return pointer to static data
 */
static char *
kbucket_to_string(const struct kbucket *kb)
{
	static char buf[80];
	char kuid[KUID_RAW_SIZE * 2 + 1];

	g_assert(kb);

	bin_to_hex_buf((char *) &kb->prefix, KUID_RAW_SIZE, kuid, sizeof kuid);

	gm_snprintf(buf, sizeof buf, "k-bucket %s (depth %d%s)",
		kuid, kb->depth, kb->ours ? ", ours" : "");

	return buf;
}

/**
 * Allocate empty node lists in the k-bucket.
 */
static void
allocate_node_lists(struct kbucket *kb)
{
	g_assert(kb);
	g_assert(kb->nodes == NULL);

	kb->nodes = walloc(sizeof *kb->nodes);

	kb->nodes->all = g_hash_table_new(sha1_hash, sha1_eq);
	kb->nodes->good = hash_list_new(knode_hash, knode_eq);
	kb->nodes->stale = hash_list_new(knode_hash, knode_eq);
	kb->nodes->pending = hash_list_new(knode_hash, knode_eq);
	kb->nodes->c_class = acct_net_create();
	kb->nodes->last_lookup = 0;
	kb->nodes->aliveness = NULL;
	kb->nodes->refresh = NULL;
}

/**
 * Forget node previously held in the routing table.
 *
 * Used to reset the status before freeing the node, to be able to assert
 * that no node from the routing table can be freed outside this file.
 */
static void
forget_node(knode_t *kn)
{
	knode_check(kn);
	g_assert(kn->status != KNODE_UNKNOWN);
	g_assert(kn->refcnt > 0);

	kn->flags &= ~KNODE_F_ALIVE;
	kn->status = KNODE_UNKNOWN;
	knode_free(kn);

	gnet_stats_count_general(GNR_DHT_ROUTING_EVICTED_NODES, 1);
}

/**
 * Hash list iterator callback.
 */
static void
forget_hashlist_node(gpointer knode, gpointer unused_data)
{
	knode_t *kn = knode;

	(void) unused_data;

	/*
	 * We do not use forget_node() here because freeing of a bucket's hash
	 * list can only happen at two well-defined times: after a bucket split
	 * (to release the parent node) or when the DHT is shutting down.
	 *
	 * In both cases (and surely in the first one), it can happen that the
	 * nodes are still referenced somewhere else, and still need to be
	 * ref-uncounted, leaving all other attributes as-is.  Unless the node
	 * is going to be disposed of, at which time we must force the status
	 * to KNODE_UNKNOWN for knode_dispose().
	 */

	if (DHT_BOOT_SHUTDOWN == GNET_PROPERTY(dht_boot_status))
		kn->status = KNODE_UNKNOWN;		/* No longer in route table */
	else if (1 == kn->refcnt)
		kn->status = KNODE_UNKNOWN;		/* For knode_dispose() */

	knode_free(kn);
}

/**
 * Free bucket's hashlist.
 */
static void
free_node_hashlist(hash_list_t *hl)
{
	g_assert(hl != NULL);

	hash_list_foreach(hl, forget_hashlist_node, NULL);
	hash_list_free(&hl);
}

/**
 * Free node lists from the k-bucket.
 */
static void
free_node_lists(struct kbucket *kb)
{
	g_assert(kb);

	if (kb->nodes) {
		struct kbnodes *knodes = kb->nodes;

		check_leaf_list_consistency(kb, knodes->good, KNODE_GOOD);
		check_leaf_list_consistency(kb, knodes->stale, KNODE_STALE);
		check_leaf_list_consistency(kb, knodes->pending, KNODE_PENDING);

		/* These cannot be NULL when kb->nodes is allocated */
		free_node_hashlist(knodes->good);
		free_node_hashlist(knodes->stale);
		free_node_hashlist(knodes->pending);
		knodes->good = knodes->stale = knodes->pending = NULL;

		g_assert(knodes->all != NULL);

		/*
		 * All the nodes listed in that table were actually also held in
		 * one of the above hash lists.  Since we expect those lists to
		 * all be empty, it means this table is now referencing freed objects.
		 */

		g_hash_table_destroy(knodes->all);
		knodes->all = NULL;

		acct_net_free(&knodes->c_class);
		cq_cancel(callout_queue, &knodes->aliveness);
		cq_cancel(callout_queue, &knodes->refresh);
		wfree(knodes, sizeof *knodes);
		kb->nodes = NULL;
	}
}

/**
 * Install periodic alive checking for bucket.
 */
static void
install_alive_check(struct kbucket *kb)
{
	int delay;
	int adj;

	g_assert(is_leaf(kb));

	/*
	 * Passive node need not refresh as often since it is not critical
	 * to be able to return good nodes to others: they don't answer RPCs.
	 * All that matters is that they keep some good nodes to be able to
	 * initiate lookups.
	 */

	delay = dht_is_active() ? ALIVE_PERIOD_MS : ALIVE_PERIOD_PASV_MS;

	/*
	 * Adjust delay randomly by +/- 5% to avoid callbacks firing at the
	 * same time for all the buckets.
	 */

	adj = ALIVE_PERIOD_MS / 10;
	adj = adj / 2 - random_value(adj);

	kb->nodes->aliveness =
		cq_insert(callout_queue, delay + adj, bucket_alive_check, kb);
}

/**
 * Install periodic refreshing of bucket.
 */
static void
install_bucket_refresh(struct kbucket *kb)
{
	int period = REFRESH_PERIOD;
	time_delta_t elapsed;

	g_assert(is_leaf(kb));

	/*
	 * Our bucket must be refreshed more often, so that we always have a
	 * complete view of our closest subtree.
	 *
	 * If we are passive (not responding to RPC calls) then it does not
	 * matter as much and our bucket does not necessarily need to be refreshed
	 * more often.
	 */

	STATIC_ASSERT(OUR_REFRESH_PERIOD < REFRESH_PERIOD);

	if (kb->ours && dht_is_active())
		period = OUR_REFRESH_PERIOD;

	/*
	 * After a bucket split, each child inherits from its parent's last lookup
	 * time.  We can therefore schedule the bucket refresh earlier if no
	 * lookups were done recently.
	 */

	elapsed = delta_time(tm_time(), kb->nodes->last_lookup);

	if (elapsed >= period)
		kb->nodes->refresh = cq_insert(callout_queue, 1, bucket_refresh, kb);
	else {
		int delay = (period - elapsed) * 1000;
		int adj;

		/*
		 * Adjust delay randomly by +/- 5% to avoid callbacks firing at the
		 * same time for all the buckets.
		 */

		adj = delay / 10;
		adj = adj / 2 - random_value(adj);

		kb->nodes->refresh =
			cq_insert(callout_queue, delay + adj, bucket_refresh, kb);
	}
}

/**
 * Recursively perform action on the bucket.
 */
static void
recursively_apply(
	struct kbucket *r, void (*f)(struct kbucket *kb, gpointer u), gpointer u)
{
	if (r == NULL)
		return;

	recursively_apply(r->zero, f, u);
	recursively_apply(r->one, f, u);
	(*f)(r, u);
}

/**
 * A new KUID is only generated if needed.
 */
void
dht_allocate_new_kuid_if_needed(void)
{
	kuid_t buf;

	/*
	 * Only generate a new KUID for this servent if all entries are 0 or
	 * if they do not want a sticky KUID.
	 *
	 * It will not be possible to run a Kademlia node with ID = 0.  That's OK.
	 */

	gnet_prop_get_storage(PROP_KUID, buf.v, sizeof buf.v);

	if (kuid_is_blank(&buf) || !GNET_PROPERTY(sticky_kuid)) {
		if (GNET_PROPERTY(dht_debug)) g_debug("generating new DHT node ID");
		kuid_random_fill(&buf);
		gnet_prop_set_storage(PROP_KUID, buf.v, sizeof buf.v);
	}

	our_kuid = kuid_get_atom(&buf);

	if (GNET_PROPERTY(dht_debug))
		g_debug("DHT local node ID is %s", kuid_to_hex_string(our_kuid));
}

/**
 * Notification callback of bucket refreshes.
 */
static void
bucket_refresh_status(const kuid_t *kuid, lookup_error_t error, gpointer arg)
{
	struct kbucket *kb = arg;

	/*
	 * Handle disabling of DHT whilst we were busy looking.
	 */

	if (NULL == root || LOOKUP_E_CANCELLED == error) {
		if (GNET_PROPERTY(dht_debug))
			g_debug("DHT disabled during bucket refresh");
		return;
	}

	if (GNET_PROPERTY(dht_debug) || GNET_PROPERTY(dht_lookup_debug)) {
		g_debug("DHT bucket refresh with %s "
			"for %s %s (good: %u, stale: %u, pending: %u) completed: %s",
			kuid_to_hex_string(kuid),
			is_leaf(kb) ? "leaf" : "split", kbucket_to_string(kb),
			safe_list_count(kb, KNODE_GOOD), safe_list_count(kb, KNODE_STALE),
			safe_list_count(kb, KNODE_PENDING),
			lookup_strerror(error));
	}

	gnet_stats_count_general(GNR_DHT_COMPLETED_BUCKET_REFRESH, 1);
}

/**
 * Issue a bucket refresh, if needed.
 */
static void
dht_bucket_refresh(struct kbucket *kb, gboolean forced)
{
	kuid_t id;

	g_assert(is_leaf(kb));

	/*
	 * If we are not completely bootstrapped, do not launch the refresh.
	 */

	if (GNET_PROPERTY(dht_boot_status) != DHT_BOOT_COMPLETED) {
		if (GNET_PROPERTY(dht_debug))
			g_warning("DHT not fully bootstrapped, denying %srefresh of %s "
				"(good: %u, stale: %u, pending: %u)",
				forced ? "forced " : "",
				kbucket_to_string(kb), list_count(kb, KNODE_GOOD),
				list_count(kb, KNODE_STALE), list_count(kb, KNODE_PENDING));
		return;
	}

	/*
	 * If we are a full non-splitable bucket, we will gain nothing by issueing
	 * a node lookup: if we get more hosts, they will not replace the good
	 * ones we have, and the bucket will not get split.  Save bandwidth and
	 * rely on periodic aliveness checks to spot stale nodes..
	 */

	if (list_count(kb, KNODE_GOOD) == K_BUCKET_GOOD && !is_splitable(kb)) {
		gnet_stats_count_general(GNR_DHT_DENIED_UNSPLITABLE_BUCKET_REFRESH, 1);
		if (GNET_PROPERTY(dht_debug))
			g_debug("DHT denying %srefresh of non-splitable full %s "
				"(good: %u, stale: %u, pending: %u)",
				forced ? "forced " : "",
				kbucket_to_string(kb), list_count(kb, KNODE_GOOD),
				list_count(kb, KNODE_STALE), list_count(kb, KNODE_PENDING));
		return;
	}

	if (GNET_PROPERTY(dht_debug)) {
		g_debug("DHT initiating %srefresh of %ssplitable %s "
			"(good: %u, stale: %u, pending: %u)",
			forced ? "forced " : "",
			is_splitable(kb) ? "" : "non-", kbucket_to_string(kb),
			list_count(kb, KNODE_GOOD), list_count(kb, KNODE_STALE),
			list_count(kb, KNODE_PENDING));
	}

	if (forced) {
		gnet_stats_count_general(GNR_DHT_FORCED_BUCKET_REFRESH, 1);
	}

	/*
	 * Generate a random KUID falling within this bucket's range.
	 */

	kuid_random_within(&id, &kb->prefix, kb->depth);

	if (GNET_PROPERTY(dht_debug))
		g_debug("DHT selected random KUID is %s", kuid_to_hex_string(&id));

	g_assert(dht_find_bucket(&id) == kb);

	/*
	 * Launch refresh.
	 *
	 * We're more aggressive for our k-bucket because we do not want to
	 * end the lookup when we have k items in our path: we really want
	 * to find the closest node we can.
	 *
	 * Likewise for forced refreshes, we want to converge to the random KUID
	 * in order to hopefully fill the bucket, somehow.
	 */

	if (kb->ours || forced)
		(void) lookup_find_node(&id, NULL, bucket_refresh_status, kb);
	else
		(void) lookup_bucket_refresh(&id, bucket_refresh_status, kb);
}

/**
 * Structure used to control bootstrap completion.
 */
struct bootstrap {
	kuid_t id;			/**< Random ID to look up */
	kuid_t current;		/**< Current prefix */
	int bits;			/**< Meaningful prefix, in bits */
};

static void bootstrap_completion_status(
	const kuid_t *kuid, lookup_error_t error, gpointer arg);

/**
 * Iterative bootstrap step.
 */
static void
completion_iterate(struct bootstrap *b)
{
	kuid_flip_nth_leading_bit(&b->current, b->bits - 1);
	kuid_random_within(&b->id, &b->current, b->bits);

	if (!lookup_find_node(&b->id, NULL, bootstrap_completion_status, b)) {
		if (GNET_PROPERTY(dht_debug))
			g_warning("DHT unable to complete bootstrapping");
		
		wfree(b, sizeof *b);
		return;
	}

	if (GNET_PROPERTY(dht_debug))
		g_warning("DHT completing bootstrap with KUID %s (%d bit%s)",
			kuid_to_hex_string(&b->id), b->bits, 1 == b->bits ? "" : "s");
}

/**
 * Notification callback of lookup of our own ID during DHT bootstrapping.
 */
static void
bootstrap_completion_status(
	const kuid_t *kuid, lookup_error_t error, gpointer arg)
{
	struct bootstrap *b = arg;

	/*
	 * Handle disabling of DHT whilst we were busy looking.
	 */

	if (NULL == root || LOOKUP_E_CANCELLED == error) {
		wfree(b, sizeof *b);
		if (GNET_PROPERTY(dht_debug))
			g_warning("DHT disabled during bootstrap");
		return;
	}

	if (GNET_PROPERTY(dht_debug) || GNET_PROPERTY(dht_lookup_debug))
		g_debug("DHT bootstrap with ID %s (%d bit%s) done: %s",
			kuid_to_hex_string(kuid), b->bits, 1 == b->bits ? "" : "s",
			lookup_strerror(error));

	/*
	 * If we were looking for just one bit, we're done.
	 */

	if (1 == b->bits) {
		wfree(b, sizeof *b);

		if (GNET_PROPERTY(dht_debug))
			g_debug("DHT now completely bootstrapped");

		gnet_prop_set_guint32_val(PROP_DHT_BOOT_STATUS, DHT_BOOT_COMPLETED);
		return;
	}

	if (LOOKUP_E_OK == error || LOOKUP_E_PARTIAL == error)
		b->bits--;

	completion_iterate(b);
}

/**
 * Complete the bootstrapping of the routing table by requesting IDs
 * futher and further away from ours.
 *
 * To avoid a sudden burst of activity, we're doing that iteratively, waiting
 * for the previous lookup to complete before launching the next one.
 */
static void
dht_complete_bootstrap(void)
{
	struct bootstrap *b;
	struct kbucket *ours;

	ours = dht_find_bucket(our_kuid);

	g_assert(ours->depth);

	b = walloc(sizeof *b);
	b->current = ours->prefix;		/* Struct copy */
	b->bits = ours->depth;

	gnet_prop_set_guint32_val(PROP_DHT_BOOT_STATUS, DHT_BOOT_COMPLETING);
	keys_update_kball();		/* We know enough to compute the k-ball */
	completion_iterate(b);
}

/**
 * Notification callback of lookup of our own ID during DHT bootstrapping.
 */
static void
bootstrap_status(const kuid_t *kuid, lookup_error_t error, gpointer unused_arg)
{
	(void) unused_arg;

	if (GNET_PROPERTY(dht_debug) || GNET_PROPERTY(dht_lookup_debug))
		g_debug("DHT bootstrapping via our own ID %s completed: %s",
			kuid_to_hex_string(kuid),
			lookup_strerror(error));

	bootstrapping = FALSE;

	/*
	 * Handle disabling of DHT whilst we were busy looking.
	 */

	if (NULL == root || LOOKUP_E_CANCELLED == error) {
		if (GNET_PROPERTY(dht_debug))
			g_warning("DHT disabled during bootstrap");
		return;
	}

	if (GNET_PROPERTY(dht_debug))
		g_debug("DHT bootstrapping was %s seeded",
			dht_seeded() ? "successfully" : "not fully");

	/*
	 * To complete the bootstrap, we need to get a better knowledge of all the
	 * buckets futher away than ours.
	 */

	if (dht_seeded())
		dht_complete_bootstrap();
	else {
		kuid_t id;

		random_bytes(id.v, sizeof id.v);

		if (GNET_PROPERTY(dht_debug))
			g_debug("DHT improving bootstrap with random KUID is %s",
			kuid_to_hex_string(&id));

		bootstrapping =
			NULL != lookup_find_node(&id, NULL, bootstrap_status, NULL);
	}
}

/**
 * Attempt DHT bootstrapping.
 */
void
dht_attempt_bootstrap(void)
{
	/*
	 * If the DHT is not initialized, ignore silently.
	 */

	if (NULL == root)
		return;

	/*
	 * If we are already completely bootstrapped, ignore.
	 */

	if (DHT_BOOT_COMPLETED == GNET_PROPERTY(dht_boot_status))
		return;

	bootstrapping = TRUE;

	/*
	 * Lookup our own ID, discarding results as all we want is the side
	 * effect of filling up our routing table with the k-closest nodes
	 * to our ID.
	 */

	if (!lookup_find_node(our_kuid, NULL, bootstrap_status, NULL)) {
		if (GNET_PROPERTY(dht_debug))
			g_debug("DHT bootstrapping impossible: routing table empty");

		bootstrapping = FALSE;
		gnet_prop_set_guint32_val(PROP_DHT_BOOT_STATUS, DHT_BOOT_NONE);
	} else {
		gnet_prop_set_guint32_val(PROP_DHT_BOOT_STATUS, DHT_BOOT_OWN);
	}

	/* XXX set DHT property status to "bootstrapping" -- red icon */
}

/**
 * Runtime (re)-initialization of the DHT.
 * If UDP or the DHT is not enabled, do nothing.
 */
void
dht_initialize(gboolean post_init)
{
	size_t i;

	if (!initialized)
		return;				/* dht_init() not called yet */

	if (!dht_enabled()) {
		/* UDP or DHT not both enabled */
		if (GNET_PROPERTY(dht_debug)) {
			g_debug("DHT will not initialize: UDP %s, DHT %s, port %u",
				GNET_PROPERTY(enable_udp) ? "on" : "off",
				GNET_PROPERTY(enable_dht) ? "on" : "off",
				GNET_PROPERTY(listen_port));
		}
		return;
	}

	if (root != NULL) {
		if (GNET_PROPERTY(dht_debug))
			g_debug("DHT already initialized");
		return;				/* already initialized */
	}

	if (GNET_PROPERTY(dht_debug))
		g_debug("DHT initializing (%s init)",
			post_init ? "post" : "first");

	dht_allocate_new_kuid_if_needed();

	/*
	 * Allocate root node for the routing table.
	 */

	root = walloc0(sizeof *root);
	root->ours = TRUE;
	allocate_node_lists(root);
	install_alive_check(root);
	install_bucket_refresh(root);

	stats.buckets++;
	gnet_stats_count_general(GNR_DHT_ROUTING_BUCKETS, +1);
	stats.leaves++;
	gnet_stats_count_general(GNR_DHT_ROUTING_LEAVES, +1);
	for (i = 0; i < K_REGIONS; i++) {
		stats.network[i].others = hash_list_new(other_size_hash, other_size_eq);
	}
	stats.lookdata = statx_make_nodata();
	stats.netdata = statx_make_nodata();

	g_assert(0 == stats.good);

	gnet_prop_set_guint32_val(PROP_DHT_BOOT_STATUS, DHT_BOOT_NONE);

	dht_route_retrieve();

	kmsg_init();
	dht_rpc_init();
	lookup_init();
	ulq_init();
	token_init();
	keys_init();
	values_init();
	publish_init();
	roots_init();
	tcache_init();
	stable_init();

	if (post_init)
		dht_attempt_bootstrap();
}

/**
 * Reset this node's KUID.
 */
void
dht_reset_kuid(void)
{
	kuid_t buf;
	kuid_zero(&buf);
	gnet_prop_set_storage(PROP_KUID, buf.v, sizeof buf.v);
}

/**
 * Initialize the whole DHT management.
 */
void
dht_init(void)
{
	initialized = TRUE;
	gnet_prop_set_guint32_val(PROP_DHT_BOOT_STATUS, DHT_BOOT_NONE);

	/*
	 * If the DHT is disabled at startup time, clear the KUID.
	 * A new one will be re-allocated the next time it is enabled.
	 */

	if (!GNET_PROPERTY(enable_dht)) {
		dht_reset_kuid();
		return;
	}

	dht_initialize(FALSE);		/* Do not attempt bootstrap yet */
}

/**
 * Does the specified bucket manage the KUID?
 */
static gboolean
dht_bucket_manages(struct kbucket *kb, const kuid_t *id)
{
	int bits = kb->depth;
	int i;

	for (i = 0; i < KUID_RAW_SIZE && bits > 0; i++, bits -= 8) {
		guchar mask = 0xff;
	
		if (bits < 8)
			mask = ~((1 << (8 - bits)) - 1) & 0xff;

		if ((kb->prefix.v[i] & mask) != (id->v[i] & mask))
			return FALSE;
	}

	/*
	 * We know that the prefix matched.  Now we have a real match only
	 * if there are no children.
	 */

	return kb->zero == NULL && kb->one == NULL;
}

/**
 * Given a depth within 0 and K_BUCKET_MAX_DEPTH, locate the byte in the
 * KUID and the mask that allows to test that bit.
 */
static inline void
kuid_position(guchar depth, int *byte, guchar *mask)
{
	g_assert(depth <= K_BUCKET_MAX_DEPTH);

	*byte = depth >> 3;					/* depth / 8 */
	*mask = 0x80 >> (depth & 0x7);		/* depth % 8 */
}

/**
 * Find bucket responsible for handling the given KUID.
 */
static struct kbucket *
dht_find_bucket(const kuid_t *id)
{
	int i;
	struct kbucket *kb = root;
	struct kbucket *result;

	for (i = 0; i < KUID_RAW_SIZE; i++) {
		guchar mask;
		guchar val = id->v[i];
		int j;

		for (j = 0, mask = 0x80; j < 8; j++, mask >>= 1) {
			result = (val & mask) ? kb->one : kb->zero;

			if (result == NULL)
				goto found;		/* Found the leaf of the tree */

			kb = result;		/* Will need to test one level beneath */
		}
	}

	/*
	 * It's not possible to come here because at some point above we'll reach
	 * a leaf node where there is no successor, whatever the bit is...  This
	 * is guaranteeed at a depth of 160.  Hence the following assertion.
	 */

	g_assert_not_reached();

	return NULL;

	/*
	 * Found the bucket, assert it is a leaf node.
	 */

found:

	g_assert(is_leaf(kb));
	g_assert(dht_bucket_manages(kb, id));

	return kb;
}

/**
 * Get number of class C networks identical to that of the node which are
 * already held in the k-bucket in any of the lists (good, pending, stale).
 */
static int
c_class_get_count(knode_t *kn, struct kbucket *kb)
{
	knode_check(kn);
	g_assert(kb);
	g_assert(is_leaf(kb));
	g_assert(kb->nodes->c_class);

	if (host_addr_net(kn->addr) != NET_TYPE_IPV4)
		return 0;

	return acct_net_get(kb->nodes->c_class, kn->addr, NET_CLASS_C_MASK);
}

/**
 * Update count of class C networks in the k-bucket when node is added
 * or removed.
 *
 * @param kn	the node added or removed
 * @param kb	the k-bucket into wich the node lies
 * @param pmone	plus or minus one
 */
static void
c_class_update_count(knode_t *kn, struct kbucket *kb, int pmone)
{
	knode_check(kn);
	g_assert(kb);
	g_assert(is_leaf(kb));
	g_assert(kb->nodes->c_class);
	g_assert(pmone == +1 || pmone == -1);

	if (host_addr_net(kn->addr) != NET_TYPE_IPV4)
		return;

	acct_net_update(kb->nodes->c_class, kn->addr, NET_CLASS_C_MASK, pmone);
}

/**
 * Total amount of nodes held in bucket (all lists).
 */
static guint
bucket_count(const struct kbucket *kb)
{
	g_assert(kb->nodes);
	g_assert(kb->nodes->all);

	return g_hash_table_size(kb->nodes->all);
}

/**
 * Assert consistent lists in bucket.
 */
static void
check_leaf_bucket_consistency(const struct kbucket *kb)
{
	guint total;
	guint good;
	guint stale;
	guint pending;

	g_assert(is_leaf(kb));

	total = bucket_count(kb);
	good = hash_list_length(kb->nodes->good);
	stale = hash_list_length(kb->nodes->stale);
	pending = hash_list_length(kb->nodes->pending);

	g_assert(good + stale + pending == total);

	check_leaf_list_consistency(kb, kb->nodes->good, KNODE_GOOD);
	check_leaf_list_consistency(kb, kb->nodes->stale, KNODE_STALE);
	check_leaf_list_consistency(kb, kb->nodes->pending, KNODE_PENDING);
}

/**
 * Context for split_among()
 */
struct node_balance {
	struct kbucket *zero;
	struct kbucket *one;
	int byte;
	guchar mask;
};

/**
 * Hash table iterator for bucket splitting.
 */
static void
split_among(gpointer key, gpointer value, gpointer user_data)
{
	kuid_t *id = key;
	knode_t *kn = value;
	struct node_balance *nb = user_data;
	struct kbucket *target;
	hash_list_t *hl;

	knode_check(kn);
	g_assert(id == kn->id);

	target = (id->v[nb->byte] & nb->mask) ? nb->one : nb->zero;

	if (GNET_PROPERTY(dht_debug) > 1)
		g_debug("DHT splitting %s to bucket \"%s\" (depth %d, %s ours)",
			knode_to_string(kn), target == nb->one ? "one" : "zero",
			target->depth, target->ours ? "is" : "not");

	hl = list_for(target, kn->status);

	g_assert(hash_list_length(hl) < list_maxsize_for(kn->status));

	hash_list_append(hl, knode_refcnt_inc(kn));
	g_hash_table_insert(target->nodes->all, kn->id, kn);
	c_class_update_count(kn, target, +1);

	check_leaf_list_consistency(target, hl, kn->status);
}

/**
 * Allocate new child for bucket.
 */
static struct kbucket *
allocate_child(struct kbucket *parent)
{
	struct kbucket *child;

	child = walloc0(sizeof *child);
	child->parent = parent;
	child->prefix = parent->prefix;
	child->depth = parent->depth + 1;
	child->split_depth = parent->split_depth;
	allocate_node_lists(child);
	child->nodes->last_lookup = parent->nodes->last_lookup;

	return child;
}

/**
 * Split k-bucket, dispatching the nodes it contains to the "zero" and "one"
 * children depending on their KUID bit at this depth.
 */
static void
dht_split_bucket(struct kbucket *kb)
{
	struct kbucket *one, *zero;
	int byte;
	guchar mask;
	struct node_balance balance;

	g_assert(kb);
	g_assert(kb->depth < K_BUCKET_MAX_DEPTH);
	g_assert(is_leaf(kb));
	check_leaf_list_consistency(kb, kb->nodes->good, KNODE_GOOD);
	check_leaf_list_consistency(kb, kb->nodes->stale, KNODE_STALE);
	check_leaf_list_consistency(kb, kb->nodes->pending, KNODE_PENDING);

	if (GNET_PROPERTY(dht_debug))
		g_debug("DHT splitting %s from %s subtree",
			kbucket_to_string(kb),
			is_among_our_closest(kb) ? "closest" : "further");

	kb->one = one = allocate_child(kb);
	kb->zero = zero = allocate_child(kb);

	/*
	 * See which one of our two children is within our tree.
	 */

	kuid_position(kb->depth, &byte, &mask);

	one->prefix.v[byte] |= mask;	/* This is "one", prefix for "zero" is 0 */

	if (our_kuid->v[byte] & mask) {
		if (kb->ours) {
			one->ours = TRUE;
			zero->split_depth = zero->depth;
		}
	} else {
		if (kb->ours) {
			zero->ours = TRUE;
			one->split_depth = one->depth;
		}
	}

	/*
	 * Install period timers for children once it is known which of the
	 * buckets is becoming ours.
	 */

	install_alive_check(kb->zero);
	install_bucket_refresh(kb->zero);
	install_alive_check(kb->one);
	install_bucket_refresh(kb->one);

	if (GNET_PROPERTY(dht_debug) > 2) {
		const char *tag;
		tag = kb->split_depth ? "left our tree at" : "in our tree since";
		g_debug("DHT split byte=%d mask=0x%x, %s depth %d",
			byte, mask, tag, kb->split_depth);
		g_debug("DHT split \"zero\" k-bucket is %s (depth %d, %s ours)",
			kuid_to_hex_string(&zero->prefix), zero->depth,
			zero->ours ? "is" : "not");
		g_debug("DHT split \"one\" k-bucket is %s (depth %d, %s ours)",
			kuid_to_hex_string(&one->prefix), one->depth,
			one->ours ? "is" : "not");
	}

	/*
	 * Now balance all the nodes from the parent bucket to the proper one.
	 */

	balance.one = one;
	balance.zero = zero;
	balance.byte = byte;
	balance.mask = mask;

	g_hash_table_foreach(kb->nodes->all, split_among, &balance);

	g_assert(bucket_count(kb) == bucket_count(zero) + bucket_count(one));

	free_node_lists(kb);			/* Parent bucket is now empty */

	g_assert(NULL == kb->nodes);	/* No longer a leaf node */
	g_assert(kb->one);
	g_assert(kb->zero);
	check_leaf_bucket_consistency(kb->one);
	check_leaf_bucket_consistency(kb->zero);

	/*
	 * Update statistics.
	 */

	stats.buckets += 2;
	stats.leaves++;					/* +2 - 1 == +1 */

	gnet_stats_count_general(GNR_DHT_ROUTING_BUCKETS, +2);
	gnet_stats_count_general(GNR_DHT_ROUTING_LEAVES, +1);
	
	if (stats.max_depth < kb->depth + 1) {
		stats.max_depth = kb->depth + 1;
		gnet_stats_set_general(GNR_DHT_ROUTING_MAX_DEPTH, stats.max_depth);
	}
}

/**
 * Add node to k-bucket with proper status.
 */
static void
add_node(struct kbucket *kb, knode_t *kn, knode_status_t new)
{
	hash_list_t *hl = list_for(kb, new);

	knode_check(kn);
	g_assert(KNODE_UNKNOWN == kn->status);
	g_assert(hash_list_length(hl) < list_maxsize_for(new));
	g_assert(new != KNODE_UNKNOWN);

	kn->status = new;
	hash_list_append(hl, knode_refcnt_inc(kn));
	g_hash_table_insert(kb->nodes->all, kn->id, kn);
	c_class_update_count(kn, kb, +1);
	stats.dirty = TRUE;

	if (GNET_PROPERTY(dht_debug) > 2)
		g_debug("DHT added new node %s to %s",
			knode_to_string(kn), kbucket_to_string(kb));

	check_leaf_list_consistency(kb, hl, new);
}

/**
 * Try to add node into the routing table at the specified bucket, or at
 * a bucket underneath if we decide to split it.
 *
 * If the bucket that should manage the node is already full and it cannot
 * be split further, we need to see whether we don't have stale nodes in
 * there.  In which case the addition is pending, until we know for sure.
 *
 * @return TRUE if we added the node to the table.
 */
static gboolean
dht_add_node_to_bucket(knode_t *kn, struct kbucket *kb, gboolean traffic)
{
	gboolean added = FALSE;

	knode_check(kn);
	g_assert(is_leaf(kb));
	g_assert(kb->nodes->all != NULL);
	g_assert(!g_hash_table_lookup(kb->nodes->all, kn->id));

	/*
	 * Not enough good entries for the bucket, add at tail of list
	 * (most recently seen).
	 */

	if (hash_list_length(kb->nodes->good) < K_BUCKET_GOOD) {
		add_node(kb, kn, KNODE_GOOD);
		stats.good++;
		gnet_stats_count_general(GNR_DHT_ROUTING_GOOD_NODES, +1);
		added = TRUE;
		goto done;
	}

	/*
	 * The bucket is full with good entries, split it first if possible.
	 */

	while (is_splitable(kb)) {
		int byte;
		guchar mask;

		dht_split_bucket(kb);
		kuid_position(kb->depth, &byte, &mask);

		kb = (kn->id->v[byte] & mask) ? kb->one : kb->zero;

		if (hash_list_length(kb->nodes->good) < K_BUCKET_GOOD) {
			add_node(kb, kn, KNODE_GOOD);
			stats.good++;
			gnet_stats_count_general(GNR_DHT_ROUTING_GOOD_NODES, +1);
			added = TRUE;
			goto done;
		}
	}

	/*
	 * We have enough "good" nodes already in this k-bucket.
	 * Put the node in the "pending" list until we have a chance to
	 * decide who among the "good" nodes is really stale...
	 *
	 * We only do so when we got the node information through incoming
	 * traffic of the host, not when the node is discovered through a
	 * lookup (listed in the RPC reply).
	 */

	if (traffic && hash_list_length(kb->nodes->pending) < K_BUCKET_PENDING) {
		add_node(kb, kn, KNODE_PENDING);
		gnet_stats_count_general(GNR_DHT_ROUTING_PENDING_NODES, +1);
		stats.pending++;
		added = TRUE;
	}

done:
	check_leaf_bucket_consistency(kb);

	return added;
}

/*
 * If there's only one reference to this node, attempt to move
 * it around if it can serve memory compaction.
 *
 * @return pointer to moved node
 */
static knode_t *
move_node(struct kbucket *kb, knode_t *kn)
{
	if (1 == knode_refcnt(kn)) {
		knode_t *moved = wmove(kn, sizeof *kn);
		if (moved != kn) {
			g_hash_table_remove(kb->nodes->all, moved->id);
			g_hash_table_insert(kb->nodes->all, moved->id, moved);
			return moved;
		}
	}

	return kn;
}

/**
 * Promote most recently seen "pending" node to the good list in the k-bucket.
 */
static void
promote_pending_node(struct kbucket *kb)
{
	knode_t *last;

	g_assert(is_leaf(kb));

	last = hash_list_tail(kb->nodes->pending);

	if (NULL == last)
		return;				/* Nothing to promote */

	g_assert(last->status == KNODE_PENDING);

	if (hash_list_length(kb->nodes->good) < K_BUCKET_GOOD) {
		knode_t *selected = NULL;

		/*
		 * Only promote a node that we know is not shutdowning.
		 * It will become unavailable soon.
		 *
		 * We iterate from the tail of the list, which is where most recently
		 * seen nodes lie.
		 */

		hash_list_iter_t *iter;
		iter = hash_list_iterator_tail(kb->nodes->pending);
		while (hash_list_iter_has_previous(iter)) {
			knode_t *kn = hash_list_iter_previous(iter);

			knode_check(kn);
			g_assert(KNODE_PENDING == kn->status);

			if (!(kn->flags & KNODE_F_SHUTDOWNING)) {
				selected = kn;
				break;
			}
		}
		hash_list_iter_release(&iter);

		if (selected) {
			time_delta_t elapsed;

			if (GNET_PROPERTY(dht_debug))
				g_debug("DHT promoting %s node %s at %s to good in %s",
					knode_status_to_string(selected->status),
					kuid_to_hex_string(selected->id),
					host_addr_port_to_string(selected->addr, selected->port),
					kbucket_to_string(kb));

			hash_list_remove(kb->nodes->pending, selected);
			list_update_stats(KNODE_PENDING, -1);

			/*
			 * If there's only one reference to this node, attempt to move
			 * it around if it can serve memory compaction.
			 */

			selected = move_node(kb, selected);

			/*
			 * Picked up node is the most recently seen pending node (at the
			 * tail of the list), but it is not necessarily the latest seen
			 * node when put among the good nodes, so we must insert at the
			 * proper position in the list.
			 */

			selected->status = KNODE_GOOD;
			hash_list_insert_sorted(kb->nodes->good, selected, knode_seen_cmp);
			list_update_stats(KNODE_GOOD, +1);

			/*
			 * If we haven't heard about the selected pending node for a while,
			 * ping it to make sure it's still alive.
			 */

			elapsed = delta_time(tm_time(), selected->last_seen);

			if (elapsed >= ALIVE_PERIOD) {
				if (GNET_PROPERTY(dht_debug)) {
					g_debug("DHT pinging promoted node (last seen %s)",
						short_time(elapsed));
				}
				if (dht_lazy_rpc_ping(selected)) {
					gnet_stats_count_general(
						GNR_DHT_ROUTING_PINGED_PROMOTED_NODES, 1);
				}
			}

			gnet_stats_count_general(GNR_DHT_ROUTING_PROMOTED_PENDING_NODES, 1);
		}
	}
}

/**
 * Check for clashing KUIDs.
 *
 * The two nodes have the same KUID, so if their IP:port differ, we have a
 * collision case.
 *
 * @return TRUE if we found a collision.
 */
static gboolean
clashing_nodes(const knode_t *kn1, const knode_t *kn2, gboolean verifying)
{
	if (!host_addr_equal(kn1->addr, kn2->addr) || kn1->port != kn2->port) {
		if (GNET_PROPERTY(dht_debug)) {
			g_warning("DHT %scollision on node %s (also at %s)",
				verifying ? "verification " : "",
				knode_to_string(kn1),
				host_addr_port_to_string(kn2->addr, kn2->port));
		}
		gnet_stats_count_general(GNR_DHT_KUID_COLLISIONS, 1);
		return TRUE;
	}

	return FALSE;
}

/**
 * Remove node from k-bucket, if present.
 */
static void
dht_remove_node_from_bucket(knode_t *kn, struct kbucket *kb)
{
	hash_list_t *hl;
	knode_t *tkn;
	gboolean was_good;

	knode_check(kn);
	g_assert(kb);
	g_assert(is_leaf(kb));

	check_leaf_bucket_consistency(kb);

	tkn = g_hash_table_lookup(kb->nodes->all, kn->id);

	if (NULL == tkn)
		return;

	/*
	 * See dht_set_node_status() for comments about tkn and kn being
	 * possible twins.
	 */

	if (tkn != kn) {
		if (clashing_nodes(tkn, kn, FALSE))
			return;
	}

	/*
	 * If node became firewalled, the KNODE_F_FIREWALLED flag has been
	 * set before calling dht_remove_node().  If we came down to here,
	 * the node was in our routing table, which means it was not firewalled
	 * at that time.
	 */

	if (kn->flags & KNODE_F_FIREWALLED)
		gnet_stats_count_general(GNR_DHT_ROUTING_EVICTED_FIREWALLED_NODES, 1);

	/*
	 * From now on, only work on "tkn" which is known to be in the
	 * routing table.
	 */

	was_good = KNODE_GOOD == tkn->status;
	hl = list_for(kb, tkn->status);

	if (hash_list_remove(hl, tkn)) {
		g_hash_table_remove(kb->nodes->all, tkn->id);
		list_update_stats(tkn->status, -1);
		c_class_update_count(tkn, kb, -1);

		if (was_good)
			promote_pending_node(kb);

		if (GNET_PROPERTY(dht_debug) > 2)
			g_debug("DHT removed %s node %s from %s",
				knode_status_to_string(tkn->status),
				knode_to_string(tkn), kbucket_to_string(kb));

		forget_node(tkn);
	}

	check_leaf_bucket_consistency(kb);
}

/**
 * Change the status of a node.
 * Can safely be called on nodes that are not in the routing table.
 */
void
dht_set_node_status(knode_t *kn, knode_status_t new)
{
	hash_list_t *hl;
	size_t maxsize;
	struct kbucket *kb;
	gboolean in_table;
	knode_status_t old;
	knode_t *tkn;

	knode_check(kn);
	g_assert(new != KNODE_UNKNOWN);

	kb = dht_find_bucket(kn->id);

	g_assert(kb);
	g_assert(kb->nodes);
	g_assert(kb->nodes->all);

	tkn = g_hash_table_lookup(kb->nodes->all, kn->id);
	in_table = NULL != tkn;

	/*
	 * We're updating a node from the routing table without changing its
	 * status: we have nothing to do.
	 */

	if (tkn == kn && kn->status == new)
		return;

	if (GNET_PROPERTY(dht_debug) > 1)
		g_debug("DHT node %s at %s (%s in table) moving from %s to %s",
			kuid_to_hex_string(kn->id),
			host_addr_port_to_string(kn->addr, kn->port),
			in_table ? (tkn == kn ? "is" : "copy") : "not",
			knode_status_to_string(((tkn && tkn != kn) ? tkn : kn)->status),
			knode_status_to_string(new));

	/*
	 * If the node has been removed from the routing table already,
	 * do NOT update the status, rather make sure it is still "unknown".
	 */

	if (!in_table) {
		g_assert(kn->status == KNODE_UNKNOWN);
		return;
	}

	/*
	 * Due to the way nodes are inserted in the routing table (upon
	 * incoming traffic reception), it is possible to have instances of
	 * the node lying in lookups and a copy in the routing table.
	 *
	 * Update the status in both if they are pointing to the same location.
	 * Otherwise it may be a case of KUID collision that we can't resolve
	 * at this level.
	 */

	if (tkn != kn) {
		if (clashing_nodes(tkn, kn, FALSE))
			return;
	}

	/*
	 * Update the twin node held in the routing table.
	 */

	check_leaf_bucket_consistency(kb);

	old = tkn->status;
	hl = list_for(kb, old);
	if (!hash_list_remove(hl, tkn))
		g_error("node %s not in its routing table list", knode_to_string(tkn));
	list_update_stats(old, -1);

	tkn->status = new;
	hl = list_for(kb, new);
	maxsize = list_maxsize_for(new);

	/*
	 * Make room in the targeted list if it is full already.
	 */

	while (hash_list_length(hl) >= maxsize) {
		knode_t *removed = hash_list_remove_head(hl);

		knode_check(removed);
		g_assert(removed->status == new);
		g_assert(removed != tkn);

		/*
		 * If removing node from the "good" list, attempt to put it back
		 * to the "pending" list to avoid dropping a good node alltogether.
		 */

		list_update_stats(new, -1);

		if (
			KNODE_GOOD == removed->status &&
			hash_list_length(kb->nodes->pending) < K_BUCKET_PENDING
		) {
			g_assert(new != KNODE_PENDING);

			removed->status = KNODE_PENDING;
			hash_list_append(kb->nodes->pending, removed);
			list_update_stats(KNODE_PENDING, +1);

			if (GNET_PROPERTY(dht_debug))
				g_debug("DHT switched %s node %s at %s to pending in %s",
					knode_status_to_string(new),
					kuid_to_hex_string(removed->id),
					host_addr_port_to_string(removed->addr, removed->port),
					kbucket_to_string(kb));
		} else {
			g_hash_table_remove(kb->nodes->all, removed->id);
			c_class_update_count(removed, kb, -1);

			if (GNET_PROPERTY(dht_debug))
				g_debug("DHT dropped %s node %s at %s from %s",
					knode_status_to_string(removed->status),
					kuid_to_hex_string(removed->id),
					host_addr_port_to_string(removed->addr, removed->port),
					kbucket_to_string(kb));

			forget_node(removed);
		}
	}

	/*
	 * Take this opportunity to move the node around if interesting.
	 */

	tkn = move_node(kb, tkn);
	hash_list_append(hl, tkn);
	list_update_stats(new, +1);

	/*
	 * If moving a node out of the good list, move the node at the tail of
	 * the pending list to the good one.
	 */

	if (old == KNODE_GOOD)
		promote_pending_node(kb);

	check_leaf_bucket_consistency(kb);
}

/**
 * Record activity of a node stored in the k-bucket.
 */
void
dht_record_activity(knode_t *kn)
{
	hash_list_t *hl;
	struct kbucket *kb;
	guint good_length;

	knode_check(kn);

	kn->last_seen = tm_time();
	kn->flags |= KNODE_F_ALIVE;

	kb = dht_find_bucket(kn->id);
	g_assert(is_leaf(kb));

	if (kn->status == KNODE_UNKNOWN) {
		g_assert(NULL == g_hash_table_lookup(kb->nodes->all, kn->id));
		return;
	}

	hl = list_for(kb, kn->status);

	g_assert(NULL != g_hash_table_lookup(kb->nodes->all, kn->id));

	/*
	 * If the "good" list is not full, try promoting the node to it.
	 * If the sum of good and stale nodes is not sufficient to fill the
	 * good list, we also set the node status to good.
	 */

	if (
		kn->status != KNODE_GOOD &&
		(good_length = hash_list_length(kb->nodes->good)) < K_BUCKET_GOOD
	) {
		guint stale_length = hash_list_length(kb->nodes->stale);

		if (stale_length + good_length >= K_BUCKET_GOOD) {
			if (kn->status == KNODE_STALE) {
				dht_set_node_status(kn, KNODE_GOOD);
				return;
			}
		} else {
			dht_set_node_status(kn, KNODE_GOOD);
			return;
		}
	}

	/*
	 * LRU list handling: move node at the end of its list.
	 */

	hash_list_moveto_tail(hl, kn);
}

/**
 * Record / update node in the routing table
 *
 * @param kn		the node we're trying to add
 * @param traffic	whether node was passively collected or we got data from it
 *
 * @return TRUE if we added the node to the table, FALSE if we rejected it or
 * if it was already present.
 */
static gboolean
record_node(knode_t *kn, gboolean traffic)
{
	struct kbucket *kb;

	knode_check(kn);

	/*
	 * Find bucket where the node will be stored.
	 */

	kb = dht_find_bucket(kn->id);

	g_assert(kb != NULL);
	g_assert(kb->nodes != NULL);

	/*
	 * Make sure we never insert ourselves.
	 */

	if (kb->ours && kuid_eq(kn->id, our_kuid)) {
		if (GNET_PROPERTY(dht_debug))
			g_warning("DHT rejecting clashing node %s: bears our KUID",
				knode_to_string(kn));
		if (!is_my_address_and_port(kn->addr, kn->port))
			gnet_stats_count_general(GNR_DHT_OWN_KUID_COLLISIONS, 1);
		return FALSE;
	}

	g_assert(!g_hash_table_lookup(kb->nodes->all, kn->id));

	/*
	 * Protect against hosts from a class C network presenting too many
	 * hosts in the same bucket space (very very unlikely, and the more
	 * so at greater bucket depths).
	 */

	if (c_class_get_count(kn, kb) >= K_BUCKET_MAX_IN_NET) {
		if (GNET_PROPERTY(dht_debug))
			g_debug("DHT rejecting new node %s at %s: "
				"too many hosts from same class-C network in %s",
				kuid_to_hex_string(kn->id),
				host_addr_port_to_string(kn->addr, kn->port),
				kbucket_to_string(kb));
		return FALSE;
	}

	/*
	 * Call dht_record_activity() before attempting to add node to have
	 * nicer logs: the "alive" flag will have been set when we stringify
	 * the knode in the logs...
	 */

	if (traffic)
		dht_record_activity(kn);

	return dht_add_node_to_bucket(kn, kb, traffic);
}

/**
 * Record traffic from a new node.
 */
void
dht_traffic_from(knode_t *kn)
{
	if (record_node(kn, TRUE) && dht_is_active())
		keys_offload(kn);

	/*
	 * If not bootstrapped yet, we just got our seed.
	 */

	if (DHT_BOOT_NONE == GNET_PROPERTY(dht_boot_status)) {
		if (GNET_PROPERTY(dht_debug))
			g_debug("DHT got a bootstrap seed with %s", knode_to_string(kn));

		gnet_prop_set_guint32_val(PROP_DHT_BOOT_STATUS, DHT_BOOT_SEEDED);
		dht_attempt_bootstrap();
	}
}

/**
 * Add node to the table after KUID verification.
 */
static void
dht_add_node(knode_t *kn)
{
	if (record_node(kn, FALSE) && dht_is_active())
		keys_offload(kn);
}

/**
 * Find node in routing table bearing the KUID.
 *
 * @return the pointer to the found node, or NULL if not present.
 */
knode_t *
dht_find_node(const kuid_t *kuid)
{
	struct kbucket *kb;

	kb = dht_find_bucket(kuid);		/* Bucket where KUID must be stored */

	g_assert(kb != NULL);
	g_assert(kb->nodes != NULL);
	g_assert(kb->nodes->all != NULL);

	return g_hash_table_lookup(kb->nodes->all, kuid);
}

/**
 * Remove node from the DHT routing table, if present.
 */
void
dht_remove_node(knode_t *kn)
{
	struct kbucket *kb;

	kb = dht_find_bucket(kn->id);
	dht_remove_node_from_bucket(kn, kb);
}

/**
 * Remove timeouting node from the bucket.
 *
 * Contrary to dht_remove_node(), we're careful not to evict the node
 * if the bucket holds less than k good entries.  Indeed, if the timeouts
 * are due to the network being disconnected, careless removal would totally
 * empty the routing table.
 */
static void
dht_remove_timeouting_node(knode_t *kn)
{
	struct kbucket *kb;

	kb = dht_find_bucket(kn->id);

	if (NULL == g_hash_table_lookup(kb->nodes->all, kn->id))
		return;			/* Node not held in routing table */

	dht_set_node_status(kn, KNODE_STALE);

	/*
	 * If bucket is full, remove the stale node, otherwise keep it around
	 * and cap it RPC timeout count to the upper threshold to avoid undue
	 * timeouts the next time an RPC is sent to the node.
	 */

	STATIC_ASSERT(KNODE_MAX_TIMEOUTS > 0);

	if (hash_list_length(kb->nodes->good) >= K_BUCKET_GOOD)
		dht_remove_node_from_bucket(kn, kb);
	else
		kn->rpc_timeouts = KNODE_MAX_TIMEOUTS;
}

/**
 * An RPC to the node timed out.
 * Can be called for a node that is no longer part of the routing table.
 */
void
dht_node_timed_out(knode_t *kn)
{
	knode_check(kn);

	/*
	 * If we're no longer connected, do not change any node status: we do
	 * not want to lose all our nodes in case the Internet link is severed.
	 */

	if (!GNET_PROPERTY(is_inet_connected)) {
		if (GNET_PROPERTY(dht_debug)) {
			g_debug("DHT not connected to Internet, "
				"ignoring RPC timeout for %s",
				knode_to_string(kn));
		}
		return;
	}

	if (++kn->rpc_timeouts >= KNODE_MAX_TIMEOUTS) {
		dht_remove_timeouting_node(kn);
	} else {
		if (kn->flags & KNODE_F_SHUTDOWNING) {
			dht_set_node_status(kn, KNODE_PENDING);
		} else {
			dht_set_node_status(kn, KNODE_STALE);
		}
	}
}

/**
 * Periodic check of live contacts in the "good" list and all the "stale"
 * contacts that can be recontacted.
 */
static void
bucket_alive_check(cqueue_t *unused_cq, gpointer obj)
{
	struct kbucket *kb = obj;
	hash_list_iter_t *iter;
	time_t now = tm_time();
	guint good_and_stale;

	(void) unused_cq;

	g_assert(is_leaf(kb));

	/*
	 * Re-instantiate the periodic callback for next time.
	 */

	install_alive_check(kb);

	if (GNET_PROPERTY(dht_debug)) {
		g_debug("DHT starting alive check on %s "
			"(good: %u, stale: %u, pending: %u)",
			kbucket_to_string(kb),
			list_count(kb, KNODE_GOOD), list_count(kb, KNODE_STALE),
			list_count(kb, KNODE_PENDING));
	}

	/*
	 * If the sum of good + stale nodes is less than the maximum amount
	 * of good nodes, try to promote that many pending nodes to the "good"
	 * status.
	 */

	good_and_stale = list_count(kb, KNODE_GOOD) + list_count(kb, KNODE_STALE);

	if (good_and_stale < K_BUCKET_GOOD) {
		guint missing = K_BUCKET_GOOD - good_and_stale;
		guint old_count;
		guint new_count;

		if (GNET_PROPERTY(dht_debug)) {
			g_debug("DHT missing %u good node%s (has %u + %u stale) in %s",
				missing, 1 == missing ? "" : "s",
				list_count(kb, KNODE_GOOD), list_count(kb, KNODE_STALE),
				kbucket_to_string(kb));
		}

		do {
			old_count = list_count(kb, KNODE_GOOD);
			promote_pending_node(kb);
			new_count = list_count(kb, KNODE_GOOD);
			if (new_count > old_count) {
				missing--;
			}
		} while (missing > 0 && new_count > old_count);

		if (GNET_PROPERTY(dht_debug)) {
			guint promoted = K_BUCKET_GOOD - good_and_stale - missing;
			if (promoted) {
				g_debug("DHT promoted %u pending node%s "
					"(now has %u good) in %s",
					promoted, 1 == promoted ? "" : "s",
					list_count(kb, KNODE_GOOD), kbucket_to_string(kb));
			}
		}
	}

	/*
	 * If there are less than half the maximum amount of good nodes in the
	 * bucket, force a bucket refresh.
	 */

	if (list_count(kb, KNODE_GOOD) < K_BUCKET_GOOD / 2) {
		if (GNET_PROPERTY(dht_debug)) {
			g_debug("DHT forcing refresh of %s %s",
				0 == list_count(kb, KNODE_GOOD) ? "empty" : "depleted",
				kbucket_to_string(kb));
		}
		dht_bucket_refresh(kb, TRUE);
	}

	/*
	 * Ping only the good contacts from which we haven't heard since the
	 * last check.
	 */

	iter = hash_list_iterator(kb->nodes->good);
	while (hash_list_iter_has_next(iter)) {
		knode_t *kn = hash_list_iter_next(iter);

		knode_check(kn);
		g_assert(KNODE_GOOD == kn->status);

		if (delta_time(now, kn->last_seen) < ALIVE_PERIOD)
			break;		/* List is sorted: least recently seen at the head */

		if (dht_lazy_rpc_ping(kn)) {
			gnet_stats_count_general(GNR_DHT_ALIVE_PINGS_TO_GOOD_NODES, 1);
		}
	}
	hash_list_iter_release(&iter);

	/*
	 * Ping all the stale nodes we can recontact.
	 */

	iter = hash_list_iterator(kb->nodes->stale);
	while (hash_list_iter_has_next(iter)) {
		knode_t *kn = hash_list_iter_next(iter);

		knode_check(kn);

		if (knode_can_recontact(kn)) {
			if (dht_lazy_rpc_ping(kn)) {
				gnet_stats_count_general(GNR_DHT_ALIVE_PINGS_TO_STALE_NODES, 1);
			}
		}
	}
	hash_list_iter_release(&iter);

	/*
	 * Ping all the pending nodes in "shutdowning mode" we can recontact
	 *
	 * These pending nodes are normally never considered, but we don't want
	 * to keep them as pending forever if they're dead, or we want to clear
	 * their "shutdowning" status if they're back to life.
	 */

	iter = hash_list_iterator(kb->nodes->pending);
	while (hash_list_iter_has_next(iter)) {
		knode_t *kn = hash_list_iter_next(iter);

		knode_check(kn);

		if ((kn->flags & KNODE_F_SHUTDOWNING) && knode_can_recontact(kn)) {
			if (dht_lazy_rpc_ping(kn)) {
				gnet_stats_count_general(
					GNR_DHT_ALIVE_PINGS_TO_SHUTDOWNING_NODES, 1);
			}
		}
	}
	hash_list_iter_release(&iter);

	gnet_stats_count_general(GNR_DHT_BUCKET_ALIVE_CHECK, 1);
}

/**
 * Periodic bucket refresh.
 */
static void
bucket_refresh(cqueue_t *unused_cq, gpointer obj)
{
	struct kbucket *kb = obj;

	g_assert(is_leaf(kb));

	(void) unused_cq;

	/*
	 * Re-instantiate for next time
	 */

	kb->nodes->last_lookup = tm_time();
	install_bucket_refresh(kb);

	dht_bucket_refresh(kb, FALSE);
}

/**
 * Given a PATRICIA trie containing the closest nodes we could find relative
 * to a given KUID, derive an estimation of the DHT size.
 *
 * @param pt		the PATRICIA trie holding the lookup path
 * @param kuid		the KUID that was looked for
 * @param amount	the amount of k-closest nodes they wanted
 */
static guint64
dht_compute_size_estimate(patricia_t *pt, const kuid_t *kuid, int amount)
{
	patricia_iter_t *iter;
	size_t i;
	size_t count;
	guint32 squares = 0;
	kuid_t *id;
	kuid_t dsum;
	kuid_t sq;
	kuid_t sparseness;
	kuid_t r;
	kuid_t max;
	kuid_t estimate;

#define NCNT	K_LOCAL_ESTIMATE

	count = patricia_count(pt);

	/*
	 * Here is the algorithm used to compute the size estimate.
	 *
	 * We perform a routing table lookup of the NCNT nodes closest to a
	 * given KUID.  Once we have that, we can estimate the sparseness of the
	 * results by computing:
	 *
	 *  Nodes = { node_1 .. node_n } sorted by increasing distance to the KUID
	 *  D = sum of Di*i for i = 1..NCNT and Di = distance(node_i, KUID)
	 *  S = sum of i*i for i = 1..NCNT
	 *
	 *  D/S represents the sparseness of the results.  If all results were
	 * at distance 1, 2, 3... etc, then D/S = 1.  The greater D/S, the more
	 * sparse the results are.
	 *
	 * The DHT size is then estimated by 2^160 / (D/S).
	 */

	iter = patricia_metric_iterator_lazy(pt, kuid, TRUE);
	i = 1;
	kuid_zero(&dsum);
	kuid_zero(&max);
	kuid_not(&max);			/* Max amount: 2^160 - 1 */

	STATIC_ASSERT(MAX_INT_VAL(guint32) >= NCNT * NCNT * NCNT);
	STATIC_ASSERT(MAX_INT_VAL(guint8) >= NCNT);

	while (patricia_iter_next(iter, (void *) &id, NULL, NULL)) {
		kuid_t di;
		gboolean saturated = FALSE;

		kuid_xor_distance(&di, id, kuid);

		/*
		 * If any of these operations report a carry, then we're saturating
		 * and it's time to leave our computations: the hosts are too sparse
		 * and the distance is getting too large.
		 */

		if (0 != kuid_mult_u8(&di, i)) {
			saturated = TRUE;
		} else if (kuid_add(&dsum, &di)) {
			saturated = TRUE;
		}

		squares += i * i;			/* Can't overflow due to static assert */
		i++;

		if (saturated) {
			kuid_copy(&dsum, &max);
			break;		/* DHT size too small or incomplete routing table */
		}
		if (i > NCNT)
			break;		/* Have collected enough nodes, more could overflow */
		if (i > UNSIGNED(amount))
			break;		/* Reaching not-so-close nodes in trie, abort */
	}
	patricia_iterator_release(&iter);

	g_assert(i - 1 <= count);

#undef NCNT

	kuid_set32(&sq, squares);
	kuid_divide(&dsum, &sq, &sparseness, &r);

	if (GNET_PROPERTY(dht_debug)) {
		double ds = kuid_to_double(&dsum);
		double s = kuid_to_double(&sq);

		g_debug("DHT target KUID is %s (%d node%s wanted, %u used)",
			kuid_to_hex_string(kuid), amount, 1 == amount ? "" : "s",
			(unsigned) (i - 1));
		g_debug("DHT dsum is %s = %f", kuid_to_hex_string(&dsum), ds);
		g_debug("DHT squares is %s = %f (%d)",
			kuid_to_hex_string(&sq), s, squares);

		g_debug("DHT sparseness over %u nodes is %s = %f (%f)",
			(unsigned) i - 1, kuid_to_hex_string(&sparseness),
			kuid_to_double(&sparseness), ds / s);
	}

	/*
	 * We can't divide 2^160 by the sparseness because we can't represent
	 * that number in a KUID.  We're going to divide 2^160 - 1 instead, which
	 * won't make much of a difference, and we add one (for ourselves).
	 */

	kuid_divide(&max, &sparseness, &estimate, &r);
	kuid_add_u8(&estimate, 1);

	return kuid_to_guint64(&estimate);
}

/**
 * Report DHT size estimate through property.
 */
static void
report_estimated_size(void)
{
	guint64 size = dht_size();

	if (GNET_PROPERTY(dht_debug)) {
		g_debug("DHT averaged global size estimate: %s "
			"(%d local, %d remote)",
			uint64_to_string(size), 1 + statx_n(stats.lookdata),
			statx_n(stats.netdata));
	}

	gnet_stats_set_general(GNR_DHT_ESTIMATED_SIZE, size);
}

/**
 * Update cached size estimate average, taking into account our local estimate
 * plus the other recent estimates made on other parts of the KUID space.
 */
static void
update_cached_size_estimate(void)
{
	time_t now = tm_time();
	int i;
	int count = 0;
	guint64 estimate = 0;
	int n;
	guint64 min = 0;
	guint64 max = MAX_INT_VAL(guint64);

	/*
	 * Only retain the points that fall within one standard deviation of
	 * the mean to remove obvious aberration.
	 */

	n = statx_n(stats.lookdata);
	if (n > 1) {
		guint64 sdev = (guint64) statx_sdev(stats.lookdata);
		guint64 avg = (guint64) statx_avg(stats.lookdata);
		if (sdev < avg)
			min = avg - sdev;
		max = avg + sdev;
	}

	for (i = 0; i < K_REGIONS; i++) {
		if (delta_time(now, stats.lookups[i].computed) <= ESTIMATE_LIFE) {
			guint64 val = stats.lookups[i].estimate;
			if (val >= min && val <= max) {
				estimate += val;
				count++;
			}
		}
	}

	/*
	 * We give as much weight to our local estimate as we give to the other
	 * collected data from different lookups on different parts of the
	 * KUID space because we know the subtree closest to our KUID in a much
	 * deeper and complete way, and thuse we can use much more nodes to
	 * compute that local estimate.
	 *
	 * We still need to average with other parts of the KUID space because
	 * we could be facing a density anomaly in the KUID space around our node.
	 */

	estimate += stats.local.estimate;
	count++;
	estimate /= count;

	stats.average.estimate = estimate;
	stats.average.computed = now;
	stats.average.amount = K_LOCAL_ESTIMATE;

	if (GNET_PROPERTY(dht_debug)) {
		g_debug("DHT cached average local size estimate is %s "
			"(%d point%s, skipped %d)",
			uint64_to_string2(stats.average.estimate),
			count, 1 == count ? "" : "s", n + 1 - count);
		if (n > 1) {
			g_debug(
				"DHT collected average is %.0f (%d points), sdev = %.2f",
				statx_avg(stats.lookdata), n, statx_sdev(stats.lookdata));
		}
	}

	report_estimated_size();
}

/**
 * After a node lookup for some KUID, see whether we have a recent-enough
 * DHT size estimate for that part of the ID space, and possibly recompute
 * one if it had expired.
 *
 * @param pt		the PATRICIA trie holding the lookup path
 * @param kuid		the KUID that was looked for
 * @param amount	the amount of k-closest nodes they wanted
 */
void
dht_update_subspace_size_estimate(
	patricia_t *pt, const kuid_t *kuid, int amount)
{
	guint8 subspace;
	time_t now = tm_time();
	guint64 estimate;
	size_t kept;

	/*
	 * See whether we have to trim some nodes (among the furthest).
	 */

	kept = patricia_count(pt);
	if (kept > UNSIGNED(amount))
		kept = amount;

	if (kept < MIN_ESTIMATE_NODES)
		return;

	subspace = kuid_leading_u8(kuid);

	STATIC_ASSERT(sizeof(guint8) == sizeof subspace);
	STATIC_ASSERT(K_REGIONS >= MAX_INT_VAL(guint8));

	/*
	 * If subspace is that of our KUID, we have more precise information
	 * in the routing table.
	 */

	if (kuid_leading_u8(our_kuid) == subspace)
		return;

	/*
	 * If we have recently updated an estimation for this subspace, return
	 * unless we have more data in the results (estimate will be more precise).
	 */

	if (delta_time(now, stats.lookups[subspace].computed) < ALIVE_PERIOD) {
		if (kept <= stats.lookups[subspace].amount)
			return;
	}

	estimate = dht_compute_size_estimate(pt, kuid, kept);

	if (stats.lookups[subspace].computed != 0)
		statx_remove(stats.lookdata, (double) stats.lookups[subspace].estimate);

	stats.lookups[subspace].estimate = estimate;
	stats.lookups[subspace].computed = now;
	stats.lookups[subspace].amount = kept;

	statx_add(stats.lookdata, (double) estimate);

	if (GNET_PROPERTY(dht_debug)) {
		g_debug("DHT subspace \"%02x\" estimate is %s (over %u/%d nodes)",
			subspace, uint64_to_string(estimate), (unsigned) kept, amount);
	}

	update_cached_size_estimate();
}

/**
 * Periodic cleanup of expired size estimates.
 */
static void
dht_expire_size_estimates(void)
{
	time_t now = tm_time();
	int i;

	for (i = 0; i < K_REGIONS; i++) {
		time_t stamp;

		stamp = stats.lookups[i].computed;
		if (stamp != 0 && delta_time(now, stamp) >= ESTIMATE_LIFE) {
			statx_remove(stats.lookdata, (double) stats.lookups[i].estimate);
			stats.lookups[i].computed = 0;

			if (GNET_PROPERTY(dht_debug)) {
				g_debug(
					"DHT expired subspace \"%02x\" local size estimate", i);
			}
		}

		stamp = stats.network[i].updated;
		if (stamp != 0 && delta_time(now, stamp) >= ESTIMATE_LIFE) {
			hash_list_t *hl = stats.network[i].others;

			while (hash_list_length(hl) > 0) {
				struct other_size *old = hash_list_remove_head(hl);
				statx_remove(stats.netdata, (double) old->size);
				other_size_free(old);
			}
			stats.network[i].updated = 0;

			if (GNET_PROPERTY(dht_debug)) {
				g_debug(
					"DHT expired subspace \"%02x\" remote size estimates", i);
			}
		}
	}
}

/**
 * Provide an estimation of the size of the DHT based on the information
 * we have in the routing table for nodes close to our KUID.
 *
 * The size is written in a 160-bit number, which is the maximum size of
 * the network. We use a KUID to hold it, for convenience.
 *
 * This routine is meant to be called periodically to update our own
 * estimate of the DHT size, which is what we report to others.
 */
void
dht_update_size_estimate(void)
{
	knode_t **kvec;
	int kcnt;
	patricia_t *pt;
	guint64 estimate;
	gboolean alive = TRUE;

	if (!dht_enabled())
		return;

	kvec = walloc(K_LOCAL_ESTIMATE * sizeof(knode_t *));
	kcnt = dht_fill_closest(our_kuid, kvec, K_LOCAL_ESTIMATE, NULL, TRUE);
	pt = patricia_create(KUID_RAW_BITSIZE);

	/*
	 * Normally the DHT size estimation is done on alive nodes but after
	 * startup, we may not have enough alive nodes in the routing table,
	 * so use "zombies" to perform our initial computations, until we get
	 * to know enough hosts.
	 */

	if (kcnt < K_LOCAL_ESTIMATE) {
		kcnt = dht_fill_closest(our_kuid, kvec, KDA_K, NULL, TRUE);
		if (kcnt < KDA_K) {
			alive = FALSE;
			kcnt = dht_fill_closest(our_kuid, kvec, KDA_K, NULL, FALSE);
		}
	}

	if (0 == kcnt) {
		estimate = 1;		/* 1 node: ourselves */
	} else {
		int i;

		for (i = 0; i < kcnt; i++) {
			knode_t *kn = kvec[i];
			patricia_insert(pt, kn->id, kn);
		}

		g_assert(patricia_count(pt) == UNSIGNED(kcnt));

		estimate = dht_compute_size_estimate(pt, our_kuid, kcnt);
	}

	if (GNET_PROPERTY(dht_debug)) {
		g_debug("DHT local size estimate is %s (using %d %s nodes)",
			uint64_to_string(estimate), kcnt,
			alive ? "alive" : "possibly zombie");
	}

	stats.local.computed = tm_time();
	stats.local.estimate = estimate;
	stats.local.amount = K_LOCAL_ESTIMATE;

	wfree(kvec, K_LOCAL_ESTIMATE * sizeof(knode_t *));
	patricia_destroy(pt);

	/*
	 * Update statistics.
	 */

	dht_expire_size_estimates();
	update_cached_size_estimate();
}

/**
 * Get our current DHT size estimate, which we propagate to others in PONGs.
 */
const kuid_t *
dht_get_size_estimate(void)
{
	static kuid_t size_estimate;

	if (stats.average.computed == 0)
		dht_update_size_estimate();

	kuid_set64(&size_estimate, stats.average.estimate);
	return &size_estimate;
}

/**
 * Record new DHT size estimate from another node.
 */
void
dht_record_size_estimate(knode_t *kn, kuid_t *size)
{
	guint8 subspace;
	struct other_size *os;
	gconstpointer key;
	struct other_size *data;
	hash_list_t *hl;
	guint64 estimate;

	knode_check(kn);
	g_assert(size);

	STATIC_ASSERT(sizeof(guint8) == sizeof subspace);
	STATIC_ASSERT(K_REGIONS >= MAX_INT_VAL(guint8));

	subspace = kuid_leading_u8(kn->id);
	hl = stats.network[subspace].others;
	estimate = kuid_to_guint64(size);

	os = walloc(sizeof *os);
	os->id = kuid_get_atom(kn->id);

	if (hash_list_find(hl, os, &key)) {
		/* This should happen only infrequently */
		other_size_free(os);
		data = deconstify_gpointer(key);
		if (data->size != estimate) {
			statx_remove(stats.netdata, (double) data->size);
			data->size = estimate;
			statx_add(stats.netdata, (double) estimate);
		}
		hash_list_moveto_tail(hl, key);
	} else {
		/* Common case: no stats recorded from this node yet */
		while (hash_list_length(hl) >= K_OTHER_SIZE) {
			struct other_size *old = hash_list_remove_head(hl);
			statx_remove(stats.netdata, (double) old->size);
			other_size_free(old);
		}
		os->size = estimate;
		statx_add(stats.netdata, (double) estimate);
		hash_list_append(hl, os);
	}

	stats.network[subspace].updated = tm_time();
}

/**
 * For local user information, compute the probable DHT size, consisting
 * of the average of all the recent sizes we have collected plus our own.
 */
guint64
dht_size(void)
{
	return statx_n(stats.netdata) > 0 ?
		(3 * stats.average.estimate + statx_avg(stats.netdata)) / 4 :
		stats.average.estimate;
}

/**
 * GList sort callback.
 */
static int
distance_to(gconstpointer a, gconstpointer b, gpointer user_data)
{
	const knode_t *ka = a;
	const knode_t *kb = b;
	const kuid_t *id = user_data;

	return kuid_cmp3(id, ka->id, kb->id);
}

/**
 * Fill the supplied vector `kvec' whose size is `kcnt' with the good
 * nodes from the current bucket, inserting them by increasing distance
 * to the supplied ID.
 *
 * @param id		the KUID for which we're finding the closest neighbours
 * @param kb		the bucket used
 * @param kvec		base of the "knode_t *" vector
 * @param kcnt		size of the "knode_t *" vector
 * @param exclude	the KUID to exclude (NULL if no exclusion)
 * @param alive		whether we want only know-to-be-alive nodes
 *
 * @return the amount of entries filled in the vector.
 */
static int
fill_closest_in_bucket(
	const kuid_t *id, struct kbucket *kb,
	knode_t **kvec, int kcnt, const kuid_t *exclude, gboolean alive)
{
	GList *nodes = NULL;
	GList *good;
	GList *l;
	int added;
	int available = 0;

	g_assert(id);
	g_assert(is_leaf(kb));
	g_assert(kvec);

	/*
	 * If we can determine that we do not have enough good nodes in the bucket
	 * to fill the vector, consider "pending" nodes (excluding shutdowning
	 * ones), provided we got traffic from them recently (defined by the
	 * aliveness period).
	 */

	good = hash_list_list(kb->nodes->good);

	while (good) {
		knode_t *kn = good->data;

		knode_check(kn);
		g_assert(KNODE_GOOD == kn->status);

		if (
			(!exclude || !kuid_eq(kn->id, exclude)) &&
			(!alive || (kn->flags & KNODE_F_ALIVE))
		) {
			nodes = g_list_prepend(nodes, kn);
			available++;
		}

		good = g_list_remove(good, kn);
	}

	if (available < kcnt) {
		GList *pending = hash_list_list(kb->nodes->pending);
		time_t now = tm_time();

		while (pending) {
			knode_t *kn = pending->data;

			knode_check(kn);
			g_assert(KNODE_PENDING == kn->status);

			if (
				!(kn->flags & KNODE_F_SHUTDOWNING) &&
				(!exclude || !kuid_eq(kn->id, exclude)) &&
				(!alive ||
					(
						(kn->flags & KNODE_F_ALIVE) &&
						delta_time(now, kn->last_seen) < ALIVE_PERIOD
					)
				)
			) {
				nodes = g_list_prepend(nodes, kn);
				available++;
			}

			pending = g_list_remove(pending, kn);
		}
	}

	/*
	 * Sort the candidates by increasing distance to the target KUID and
	 * insert them in the vector.
	 */

	nodes = g_list_sort_with_data(nodes, distance_to, deconstify_gpointer(id));

	for (added = 0, l = nodes; l && kcnt; l = g_list_next(l)) {
		*kvec++ = l->data;
		kcnt--;
		added++;
	}

	g_list_free(nodes);

	return added;
}

/**
 * Recursively fill the supplied vector `kvec' whose size is `kcnt' with the
 * good nodes held in the leaves under the current bucket,
 * inserting them by increasing distance to the supplied ID.
 *
 * @param id		the KUID for which we're finding the closest neighbours
 * @param kb		the bucket from which we recurse
 * @param kvec		base of the "knode_t *" vector
 * @param kcnt		size of the "knode_t *" vector
 * @param exclude	the KUID to exclude (NULL if no exclusion)
 * @param alive		whether we want only know-to-be-alive nodes
 *
 * @return the amount of entries filled in the vector.
 */
static int
recursively_fill_closest_from(
	const kuid_t *id,
	struct kbucket *kb,
	knode_t **kvec, int kcnt, const kuid_t *exclude, gboolean alive)
{
	int byte;
	guchar mask;
	struct kbucket *closest;
	int added;

	g_assert(id);
	g_assert(kb);

	if (is_leaf(kb))
		return fill_closest_in_bucket(id, kb, kvec, kcnt, exclude, alive);

	kuid_position(kb->depth, &byte, &mask);

	if ((kb->one->prefix.v[byte] & mask) == (id->v[byte] & mask)) {
		g_assert((kb->zero->prefix.v[byte] & mask) != (id->v[byte] & mask));
		closest = kb->one;
	} else {
		g_assert((kb->zero->prefix.v[byte] & mask) == (id->v[byte] & mask));
		closest = kb->zero;
	}

	added = recursively_fill_closest_from(
		id, closest, kvec, kcnt, exclude, alive);

	if (added < kcnt)
		added += recursively_fill_closest_from(id, sibling_of(closest),
			kvec + added, kcnt - added, exclude, alive);

	return added;
}

/**
 * Fill the supplied vector `kvec' whose size is `kcnt' with the knodes
 * that are the closest neighbours in the Kademlia space from a given KUID.
 *
 * @param id		the KUID for which we're finding the closest neighbours
 * @param kvec		base of the "knode_t *" vector
 * @param kcnt		size of the "knode_t *" vector
 * @param exclude	the KUID to exclude (NULL if no exclusion)
 * @param alive		whether we want only known-to-be-alive nodes
 *
 * @return the amount of entries filled in the vector.
 */
int
dht_fill_closest(
	const kuid_t *id,
	knode_t **kvec, int kcnt, const kuid_t *exclude, gboolean alive)
{
	struct kbucket *kb;
	int added;
	int wanted = kcnt;			/* Remember for tracing only */
	knode_t **base = kvec;		/* Idem */

	g_assert(id);
	g_assert(kcnt > 0);
	g_assert(kvec);

	/*
	 * Start by filling from hosts in the k-bucket of the ID.
	 */

	kb = dht_find_bucket(id);
	added = fill_closest_in_bucket(id, kb, kvec, kcnt, exclude, alive);
	kvec += added;
	kcnt -= added;

	g_assert(kcnt >= 0);

	/*
	 * Now iteratively move up to the root bucket, trying to fill more
	 * closest nodes from these buckets which are farther and farther away
	 * from the target ID.
	 */

	for (/* empty */; kb->depth && kcnt; kb = kb->parent) {
		struct kbucket *sibling = sibling_of(kb);
		int more;

		g_assert(sibling->parent == kb->parent);
		g_assert(sibling != kb);

		more = recursively_fill_closest_from(
			id, sibling, kvec, kcnt, exclude, alive);
		kvec += more;
		kcnt -= more;
		added += more;

		g_assert(kcnt >= 0);
	}

	if (GNET_PROPERTY(dht_debug) > 15) {
		g_debug("DHT found %d/%d %s nodes (excluding %s) closest to %s",
			added, wanted, alive ? "alive" : "known",
			exclude ? kuid_to_hex_string(exclude) : "nothing",
			kuid_to_hex_string2(id));

		if (GNET_PROPERTY(dht_debug) > 19) {
			int i;

			for (i = 0; i < added; i++) {
				g_debug("DHT closest[%d]: %s", i, knode_to_string(base[i]));
			}
		}
	}

	return added;
}

/**
 * Fill the supplied vector `hvec' whose size is `hcnt' with the addr:port
 * of random hosts in the routing table.
 *
 * @param hvec		base of the "gnet_host_t *" vector
 * @param hcnt		size of the "gnet_host_t *" vector
 *
 * @return the amount of entries filled in the vector.
 */
int
dht_fill_random(gnet_host_t *hvec, int hcnt)
{
	int i, j;
	int maxtry;
	map_t *seen;

	g_assert(hcnt < MAX_INT_VAL(int) / 2);

	/*
	 * If DHT was never initialized or turned off, then the root bucket was
	 * freed and there is nothing to look for.
	 */

	if (NULL == root)
		return 0;

	maxtry = hcnt + hcnt;
	seen = map_create_patricia(KUID_RAW_SIZE);

	for (i = j = 0; i < hcnt && j < maxtry; i++, j++) {
		kuid_t id;
		struct kbucket *kb;
		knode_t *kn;

		random_bytes(id.v, sizeof id.v);
		kb = dht_find_bucket(&id);
		kn = hash_list_tail(list_for(kb, KNODE_GOOD));	/* Recently seen */

		if (NULL == kn || map_contains(seen, &kb->prefix)) {
			i--;
			continue;	/* Bad luck: empty list or already seen */
		}

		gnet_host_set(&hvec[i], kn->addr, kn->port);
		map_insert(seen, &kb->prefix, NULL);
	}

	map_destroy(seen);

	return i;			/* Amount filled in vector */
}

/**
 * Invoked when a lookup is performed on the ID, so that we may update
 * the time of the last refresh in the ID's bucket.
 */
void
dht_lookup_notify(const kuid_t *id)
{
	int period;
	struct kbucket *kb;

	g_assert(id);

	kb = dht_find_bucket(id);
	kb->nodes->last_lookup = tm_time();
	period = kb->ours ? OUR_REFRESH_PERIOD : REFRESH_PERIOD;
	
	cq_resched(callout_queue, kb->nodes->refresh, period * 1000);
}

/**
 * Write node information to file.
 */
static void
write_node(const knode_t *kn, FILE *f)
{
	knode_check(kn);

	fprintf(f, "KUID %s\nVNDR %s\nVERS %u.%u\nHOST %s\nSEEN %s\nEND\n\n",
		kuid_to_hex_string(kn->id),
		vendor_code_to_string(kn->vcode.u32),
		kn->major, kn->minor,
		host_addr_port_to_string(kn->addr, kn->port),
		timestamp_utc_to_string(kn->last_seen));
}

/**
 * Store all good nodes from a leaf bucket.
 */
static void
dht_store_leaf_bucket(struct kbucket *kb, gpointer u)
{
	FILE *f = u;
	hash_list_iter_t *iter;

	if (!is_leaf(kb))
		return;

	/*
	 * All good nodes are persisted.
	 */

	iter = hash_list_iterator(kb->nodes->good);
	while (hash_list_iter_has_next(iter)) {
		const knode_t *kn;

		kn = hash_list_iter_next(iter);
		write_node(kn, f);
	}
	hash_list_iter_release(&iter);

	/*
	 * Stale nodes for which the RPC timeout condition was cleared
	 * are also elected.
	 */

	iter = hash_list_iterator(kb->nodes->stale);
	while (hash_list_iter_has_next(iter)) {
		const knode_t *kn;

		kn = hash_list_iter_next(iter);
		if (!kn->rpc_timeouts)
			write_node(kn, f);
	}
	hash_list_iter_release(&iter);
}

/**
 * Save all the good nodes from the routing table.
 */
static void
dht_route_store(void)
{
	FILE *f;
	file_path_t fp;

	file_path_set(&fp, settings_config_dir(), dht_route_file);
	f = file_config_open_write(dht_route_what, &fp);

	if (!f)
		return;

	file_config_preamble(f, "DHT nodes");

	fputs(
		"#\n"
		"# Format is:\n"
		"#  KUID <hex node ID>\n"
		"#  VNDR <vendor code>\n"
		"#  VERS <major.minor>\n"
		"#  HOST <IP and port>\n"
		"#  SEEN <last seen message>\n"
		"#  END\n"
		"#  \n\n",
		f
	);

	if (root)
		recursively_apply(root, dht_store_leaf_bucket, f);

	file_config_close(f, &fp);
	stats.dirty = FALSE;
}

/**
 * Save good nodes if table is dirty.
 */
void
dht_route_store_if_dirty(void)
{
	if (stats.dirty)
		dht_route_store();
}

/**
 * Free bucket node.
 */
static void
dht_free_bucket(struct kbucket *kb, gpointer unused_u)
{
	(void) unused_u;

	free_node_lists(kb);
	wfree(kb, sizeof *kb);
}

/**
 * Hash list iterator callback.
 */
static void
other_size_free_cb(gpointer other_size, gpointer unused_data)
{
	struct other_size *os = other_size;

	(void) unused_data;

	other_size_free(os);
}

/**
 * Shutdown the DHT.
 *
 * @param exiting	whether gtk-gnutella is exiting altogether
 */
void
dht_close(gboolean exiting)
{
	size_t i;

	/*
	 * If the DHT was never initialized, there's nothing to close.
	 */

	if (NULL == root)
		return;

	dht_route_store();

	/*
	 * Since we're shutting down the route table, we also need to shut down
	 * the RPC and lookups, which rely on the routing table.
	 */

	lookup_close(exiting);
	publish_close(exiting);
	ulq_close(exiting);
	stable_close();
	tcache_close();
	roots_close();
	values_close();
	keys_close();
	dht_rpc_close();
	token_close();
	kmsg_close();

	old_boot_status = GNET_PROPERTY(dht_boot_status);
	gnet_prop_set_guint32_val(PROP_DHT_BOOT_STATUS, DHT_BOOT_SHUTDOWN);

	recursively_apply(root, dht_free_bucket, NULL);
	root = NULL;
	kuid_atom_free_null(&our_kuid);

	for (i = 0; i < K_REGIONS; i++) {
		hash_list_t *hl = stats.network[i].others;
		if (hl)
			hash_list_foreach(hl, other_size_free_cb, NULL);
		hash_list_free(&stats.network[i].others);
	}
	statx_free(stats.lookdata);
	statx_free(stats.netdata);

	memset(&stats, 0, sizeof stats);		/* Clear all stats */
	gnet_prop_set_guint32_val(PROP_DHT_BOOT_STATUS, DHT_BOOT_NONE);
}

/***
 *** RPC calls for routing table management.
 ***/

/**
 * Structure used to keep the context of nodes that are verified: whenever
 * we get a duplicate KUID from an alien address, we verify the old address
 * and keep the new node around: if the old does not answer, we replace the
 * entry by the new one, otherwise we discard the new.
 */
struct addr_verify {
	knode_t *old;
	knode_t *new;
};

/**
 * RPC callback for the address verification.
 *
 * @param type			DHT_RPC_REPLY or DHT_RPC_TIMEOUT
 * @param kn			the replying node
 * @param function		the type of message we got (0 on TIMEOUT)
 * @param payload		the payload we got
 * @param len			the length of the payload
 * @param arg			user-defined callback parameter
 */
static void
dht_addr_verify_cb(
	enum dht_rpc_ret type,
	const knode_t *kn,
	const struct gnutella_node *unused_n,
	kda_msg_t unused_function,
	const gchar *unused_payload, size_t unused_len, gpointer arg)
{
	struct addr_verify *av = arg;

	(void) unused_n;
	(void) unused_function;
	(void) unused_payload;
	(void) unused_len;

	knode_check(kn);

	if (type == DHT_RPC_TIMEOUT || !kuid_eq(av->old->id, kn->id)) {
		/*
		 * Timeout, or the host that we probed no longer bears the KUID
		 * we had in our records for it.  Discard the old and keep the new,
		 * unless it is firewalled.
		 */

		if (GNET_PROPERTY(dht_debug))
			g_warning("DHT verification failed for node %s: %s",
				knode_to_string(av->old),
				type == DHT_RPC_TIMEOUT ?
					"ping timed out" : "replied with a foreign KUID");

		dht_remove_node(av->old);

		if (av->new->flags & KNODE_F_FIREWALLED) {
			if (GNET_PROPERTY(dht_debug))
				g_warning("DHT verification ignoring firewalled new node %s",
					knode_to_string(av->new));
		} else {
			knode_t *tkn;

			tkn = dht_find_node(av->new->id);

			if (GNET_PROPERTY(dht_debug))
				g_warning("DHT verification keeping new node %s",
					knode_to_string(av->new));

			if (NULL == tkn) {
				av->new->flags |= KNODE_F_ALIVE;	/* Got traffic earlier! */
				dht_add_node(av->new);
			} else if (clashing_nodes(tkn, av->new, TRUE)) {
				/* Logging was done in clashing_nodes() */
			} else {
				if (GNET_PROPERTY(dht_debug))
					g_warning("DHT verification found existing new node %s",
						knode_to_string(tkn));
			}
		}
	} else {
		av->old->flags &= ~KNODE_F_VERIFYING;	/* got reply from proper host */

		if (GNET_PROPERTY(dht_debug))
			g_warning("DHT verification OK, keeping old node %s",
				knode_to_string(av->old));
	}

	knode_free(av->old);
	knode_free(av->new);
	wfree(av, sizeof *av);
}

/**
 * Verify the node address when we get a conflicting one.
 *
 * It is possible that the address of the node changed, so we send a PING to
 * the old address we had decide whether it is the case (no reply or another
 * KUID will come back), or whether the new node we found has a duplicate KUID
 * (maybe intentionally).
 */
void
dht_verify_node(knode_t *kn, knode_t *new)
{
	struct addr_verify *av;

	knode_check(kn);
	knode_check(new);
	g_assert(new->refcnt == 1);
	g_assert(new->status == KNODE_UNKNOWN);
	g_assert(!(kn->flags & KNODE_F_VERIFYING));

	av = walloc(sizeof *av);

	if (GNET_PROPERTY(dht_debug))
		g_debug("DHT node %s was at %s, now %s -- verifying",
			kuid_to_hex_string(kn->id),
			host_addr_port_to_string(kn->addr, kn->port),
			host_addr_port_to_string2(new->addr, new->port));

	kn->flags |= KNODE_F_VERIFYING;
	av->old = knode_refcnt_inc(kn);
	av->new = knode_refcnt_inc(new);

	/*
	 * We use RPC_CALL_NO_VERIFY because we want to handle the verification
	 * of the address of the replying node ourselves in the callback because
	 * the "new" node bears the same KUID as the "old" one.
	 */

	dht_rpc_ping_extended(kn, RPC_CALL_NO_VERIFY, dht_addr_verify_cb, av);
}

/**
 * RPC callback for the random PING.
 *
 * @param type			DHT_RPC_REPLY or DHT_RPC_TIMEOUT
 * @param kn			the replying node
 * @param function		the type of message we got (0 on TIMEOUT)
 * @param payload		the payload we got
 * @param len			the length of the payload
 * @param arg			user-defined callback parameter
 */
static void
dht_ping_cb(
	enum dht_rpc_ret type,
	const knode_t *kn,
	const struct gnutella_node *unused_n,
	kda_msg_t unused_function,
	const gchar *unused_payload, size_t unused_len, gpointer unused_arg)
{
	(void) unused_n;
	(void) unused_function;
	(void) unused_payload;
	(void) unused_len;
	(void) unused_arg;

	if (DHT_RPC_TIMEOUT == type)
		return;

	if (GNET_PROPERTY(dht_debug))
		g_debug("DHT reply from randomly pinged %s",
			host_addr_port_to_string(kn->addr, kn->port));
}

/*
 * Send a DHT Ping to the supplied address, randomly and not more than once
 * every minute.
 */
static void
dht_ping(host_addr_t addr, guint16 port)
{
	knode_t *kn;
	vendor_code_t vc;
	static time_t last_sent = 0;
	time_t now = tm_time();

	/*
	 * Passive nodes are not part of the DHT structure, so no need to ping
	 * random hosts: our node will never become part of another's routing
	 * table unless we are active.
	 */

	if (!dht_is_active())
		return;

	/*
	 * The idea is to prevent the formation of DHT islands by using another
	 * channel (Gnutella) to propagate hosts participating to the DHT.
	 * Not more than one random ping per minute though.
	 */

	if (delta_time(now, last_sent) < 60 || (random_u32() % 100) >= 10)
		return;

	last_sent = now;

	if (GNET_PROPERTY(dht_debug))
		g_debug("DHT randomly pinging host %s",
			host_addr_port_to_string(addr, port));

	/*
	 * Build a fake Kademlia node, with an zero KUID.  This node will never
	 * be inserted in the routing table as such and will only be referenced
	 * by the callback.
	 */

	vc.u32 = T_0000;
	kn = knode_new(&kuid_null, 0, addr, port, vc, 0, 0);

	/*
	 * We do not want the RPC layer to verify the KUID of the replying host
	 * since we don't even have a valid KUID for the remote host yet!
	 * Hence the use of the RPC_CALL_NO_VERIFY control flag.
	 */

	dht_rpc_ping_extended(kn, RPC_CALL_NO_VERIFY, dht_ping_cb, NULL);
	knode_free(kn);
}

/**
 * Send a DHT ping as a probe, hoping the pong reply will help us bootstrap.
 */
static void
dht_probe(host_addr_t addr, guint16 port)
{
	knode_t *kn;
	vendor_code_t vc;
	guid_t muid;

	/*
	 * Build a fake Kademlia node, with an zero KUID.  This node will never
	 * be inserted in the routing table as such and will only be referenced
	 * by the callback.
	 *
	 * Send it a ping and wait for a reply.  When (if) it comes, it will
	 * seed the routing table and we will attempt the bootstrap.
	 */

	vc.u32 = T_0000;
	kn = knode_new(&kuid_null, 0, addr, port, vc, 0, 0);
	guid_random_muid(cast_to_gpointer(&muid));
	kmsg_send_ping(kn, &muid);
	knode_free(kn);
}

/**
 * Send a bootstrapping Kademlia PING to specified host.
 *
 * We're not even sure the address we have here is that of a valid node,
 * but getting back a valid Kademlia PONG will be enough for us to launch
 * the bootstrapping as we will know one good node!
 */
static void
dht_bootstrap(host_addr_t addr, guint16 port)
{
	/*
	 * We can be called only until we have been fully bootstrapped, but we
	 * must not continue to attempt bootstrapping from other nodes if we
	 * are already in the process of looking up our own node ID.
	 */

	if (bootstrapping)
		return;				/* Hopefully we'll be bootstrapped soon */

	if (GNET_PROPERTY(dht_debug))
		g_debug("DHT attempting bootstrap from %s",
			host_addr_port_to_string(addr, port));

	dht_probe(addr, port);
}

/**
 * Called when we get a Gnutella pong marked with a GGEP "DHT" extension.
 *
 * Bootstrap the DHT from the supplied address, if needed, otherwise
 * randomly attempt to ping the node.
 */
void
dht_bootstrap_if_needed(host_addr_t addr, guint16 port)
{
	if (!dht_enabled())
		return;

	if (dht_seeded())
		dht_ping(addr, port);
	else
		dht_bootstrap(addr, port);
}

/**
 * Collect packed IP:port DHT hosts from "DHTIPP" we get in a pong.
 */
void
dht_ipp_extract(const struct gnutella_node *n, const char *payload, int paylen)
{
	int i, cnt;

	g_assert(0 == paylen % 6);

	cnt = paylen / 6;

	if (GNET_PROPERTY(dht_debug) || GNET_PROPERTY(bootstrap_debug))
		g_debug("extracting %d DHT host%s in DHTIPP pong from %s",
			cnt, cnt == 1 ? "" : "s", node_addr(n));

	for (i = 0; i < cnt; i++) {
		host_addr_t ha;
		guint16 port;

		ha = host_addr_peek_ipv4(&payload[i * 6]);
		port = peek_le16(&payload[i * 6 + 4]);

		if (GNET_PROPERTY(bootstrap_debug) > 1)
			g_debug("BOOT collected DHT node %s from DHTIPP pong from %s",
				host_addr_to_string(ha), node_addr(n));

		dht_probe(ha, port);
	}
}

/***
 *** Parsing of persisted DHT routing table.
 ***/

typedef enum {
	DHT_ROUTE_TAG_UNKNOWN = 0,

	DHT_ROUTE_TAG_KUID,
	DHT_ROUTE_TAG_VNDR,
	DHT_ROUTE_TAG_VERS,
	DHT_ROUTE_TAG_HOST,
	DHT_ROUTE_TAG_SEEN,
	DHT_ROUTE_TAG_END,

	DHT_ROUTE_TAG_MAX
} dht_route_tag_t;

/* Amount of valid route tags, excluding the unknown placeholder tag */
#define NUM_DHT_ROUTE_TAGS	(DHT_ROUTE_TAG_MAX - 1)

static const struct dht_route_tag {
	dht_route_tag_t tag;
	const char *str;
} dht_route_tag_map[] = {
	/* Must be sorted alphabetically for dichotomic search */

#define DHT_ROUTE_TAG(x)	{ CAT2(DHT_ROUTE_TAG_,x), #x }

	DHT_ROUTE_TAG(END),
	DHT_ROUTE_TAG(HOST),
	DHT_ROUTE_TAG(KUID),
	DHT_ROUTE_TAG(SEEN),
	DHT_ROUTE_TAG(VERS),
	DHT_ROUTE_TAG(VNDR),

	/* Above line intentionally left blank (for "!}sort" in vi) */
#undef DHT_ROUTE_TAG
};

static dht_route_tag_t
dht_route_string_to_tag(const char *s)
{
	STATIC_ASSERT(G_N_ELEMENTS(dht_route_tag_map) == NUM_DHT_ROUTE_TAGS);

#define GET_ITEM(i)		dht_route_tag_map[i].str
#define FOUND(i) G_STMT_START {				\
	return dht_route_tag_map[i].tag;		\
	/* NOTREACHED */						\
} G_STMT_END

	/* Perform a binary search to find ``s'' */
	BINARY_SEARCH(const char *, s, G_N_ELEMENTS(dht_route_tag_map), strcmp,
		GET_ITEM, FOUND);

#undef FOUND
#undef GET_ITEM

	return DHT_ROUTE_TAG_UNKNOWN;
}

/**
 * Load persisted routing table from file.
 */
static void
dht_route_parse(FILE *f)
{
	bit_array_t tag_used[BIT_ARRAY_SIZE(NUM_DHT_ROUTE_TAGS + 1)];
	char line[1024];
	guint line_no = 0;
	gboolean done = FALSE;
	time_delta_t most_recent = REFRESH_PERIOD;
	time_t now = tm_time();
	patricia_t *nodes;
	patricia_iter_t *iter;
	/* Variables filled for each entry */
	host_addr_t addr;
	guint16 port;
	kuid_t kuid;
	vendor_code_t vcode = { 0 };
	time_t seen = (time_t) -1;
	guint32 major, minor;

	g_return_if_fail(f);

	bit_array_init(tag_used, NUM_DHT_ROUTE_TAGS);
	nodes = patricia_create(KUID_RAW_BITSIZE);

	while (fgets(line, sizeof line, f)) {
		const char *tag_name, *value;
		char *sp, *nl;
		dht_route_tag_t tag;

		line_no++;

		nl = strchr(line, '\n');
		if (!nl) {
			/*
			 * Line was too long or the file was corrupted or manually
			 * edited without consideration for the advertised format.
			 */

			g_warning("dht_route_parse(): "
				"line too long or missing newline in line %u", line_no);
			break;
		}
		*nl = '\0';		/* Terminate string properly */

		/* Skip comments and empty lines */

		if (*line == '#' || *line == '\0')
			continue;

		sp = strchr(line, ' ');		/* End of tag, normally */
		if (sp) {
			*sp = '\0';
			value = &sp[1];
		} else {
			value = strchr(line, '\0');		/* Tag without a value */
		}
		tag_name = line;

		tag = dht_route_string_to_tag(tag_name);
		g_assert(UNSIGNED(tag) <= NUM_DHT_ROUTE_TAGS);

		if (tag != DHT_ROUTE_TAG_UNKNOWN && !bit_array_flip(tag_used, tag)) {
			g_warning("dht_route_parse(): "
				"duplicate tag \"%s\" within entry at line %u",
				tag_name, line_no);
			goto damaged;
		}

		switch (tag) {
		case DHT_ROUTE_TAG_KUID:
			if (
				KUID_RAW_SIZE * 2 != strlen(value) ||
				KUID_RAW_SIZE != base16_decode((char *) kuid.v, sizeof kuid.v,
					value, KUID_RAW_SIZE * 2)
			)
				goto damaged;
			break;
		case DHT_ROUTE_TAG_VNDR:
			if (4 == strlen(value))
				vcode.u32 = peek_be32(value);
			else
				goto damaged;
			break;
		case DHT_ROUTE_TAG_VERS:
			if (0 != parse_major_minor(value, NULL, &major, &minor))
				goto damaged;
			else if (major > 256 || minor > 256)
				goto damaged;
			break;
		case DHT_ROUTE_TAG_HOST:
			if (!string_to_host_addr_port(value, NULL, &addr, &port))
				goto damaged;
			break;
		case DHT_ROUTE_TAG_SEEN:
			seen = date2time(value, tm_time());
			if ((time_t) -1 == seen)
				goto damaged;
			break;
		case DHT_ROUTE_TAG_END:
			{
				size_t i;

				for (i = 0; i < G_N_ELEMENTS(dht_route_tag_map); i++) {
					if (!bit_array_get(tag_used, dht_route_tag_map[i].tag)) {
						g_warning("dht_route_parse(): "
							"missing %s tag near line %u",
							dht_route_tag_map[i].str, line_no);
						goto damaged;
					}
				}
			}
			done = TRUE;
			break;
		case DHT_ROUTE_TAG_UNKNOWN:
			/* Silently ignore */
			break;
		case DHT_ROUTE_TAG_MAX:
			g_assert_not_reached();
			break;
		}

		if (done) {
			knode_t *kn;
			time_delta_t delta;

			/*
			 * Remember the delta at which we most recently saw a node.
			 */

			delta = delta_time(now, seen);
			if (delta >= 0 && delta < most_recent)
				most_recent = delta;

			kn = knode_new(&kuid, 0, addr, port, vcode, major, minor);
			kn->last_seen = seen;

			/*
			 * Add node to routing table.  If the KUID has changed since
			 * the last time the routing table was saved (e.g. they are
			 * importing a persisted file from another instance), then bucket
			 * splits will not occur in the same way and some nodes will be
			 * discarded.  It does not matter much, we should have enough
			 * good hosts to attempt a bootstrap.
			 *
			 * Since they shutdown, the bogons or hostile database could
			 * have changed.  Revalidate addresses.
			 */

			if (!knode_is_usable(kn)) {
				g_warning("DHT ignoring persisted unusable %s",
					knode_to_string(kn));
			} else {
				patricia_insert(nodes, kn->id, kn);
			}

			/* Reset state */
			done = FALSE;
			bit_array_clear_range(tag_used, 0, NUM_DHT_ROUTE_TAGS);
		}
		continue;

	damaged:
		g_warning("damaged DHT route entry at line %u, aborting", line_no);
		break;
	}

	/*
	 * Now insert the recorded nodes in topological order, so that
	 * we fill the closest subtree first and minimize the level of
	 * splitting in the furthest parts of the tree.
	 */

	iter = patricia_metric_iterator_lazy(nodes, our_kuid, TRUE);

	while (patricia_iter_has_next(iter)) {
		knode_t *tkn;
		knode_t *kn = patricia_iter_next_value(iter);
		if ((tkn = dht_find_node(kn->id))) {
			g_warning("DHT ignoring persisted dup %s (has %s already)",
				knode_to_string(kn), knode_to_string2(tkn));
		} else {
			if (!record_node(kn, FALSE)) {
				/* This can happen when the furthest subtrees are full */
				if (GNET_PROPERTY(dht_debug)) {
					g_debug("DHT ignored persisted %s", knode_to_string(kn));
				}
			}
		}
	}
	patricia_iterator_release(&iter);
	patricia_foreach(nodes, knode_patricia_free, NULL);
	patricia_destroy(nodes);

	/*
	 * If the delta is smaller than half the bucket refresh period, we
	 * can consider the table as being bootstrapped: they are restarting
	 * after an update, for instance.
	 */

	if (dht_seeded()) {
		enum dht_bootsteps boot_status =
			most_recent < REFRESH_PERIOD / 2 ?
				DHT_BOOT_COMPLETED : DHT_BOOT_SEEDED;
		if (
			old_boot_status != DHT_BOOT_NONE &&
			old_boot_status != DHT_BOOT_COMPLETED
		) {
			boot_status = old_boot_status;
		}
		gnet_prop_set_guint32_val(PROP_DHT_BOOT_STATUS, boot_status);
	}

	if (GNET_PROPERTY(dht_debug))
		g_debug("DHT after retrieval we are %s",
			boot_status_to_string(GNET_PROPERTY(dht_boot_status)));

	keys_update_kball();
	dht_update_size_estimate();
}

static const gchar node_file[] = "dht_nodes";
static const gchar file_what[] = "DHT nodes";

/**
 * Retrieve previous routing table from ~/.gtk-gnutella/dht_nodes.
 */
static void
dht_route_retrieve(void)
{
	file_path_t fp[1];
	FILE *f;

	file_path_set(fp, settings_config_dir(), node_file);
	f = file_config_open_read(file_what, fp, G_N_ELEMENTS(fp));

	if (f) {
		dht_route_parse(f);
		fclose(f);
	}
}

/* vi: set ts=4 sw=4 cindent: */
