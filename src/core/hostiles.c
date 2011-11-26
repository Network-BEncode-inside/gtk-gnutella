/*
 * Copyright (c) 2004-2011, Raphael Manfredi
 * Copyright (c) 2003, Markus Goetz
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
 * @ingroup core
 * @file
 *
 * Support for the hostiles.txt of BearShare.
 *
 * This file is based a lot on the whitelist stuff by vidar.
 *
 * @author Markus Goetz
 * @date 2003
 * @author Raphael Manfredi
 * @date 2004-2011
 */

#include "common.h"

#include "hostiles.h"
#include "settings.h"
#include "nodes.h"
#include "gnet_stats.h"

#include "dht/stable.h"

#include "lib/ascii.h"
#include "lib/atoms.h"
#include "lib/cq.h"
#include "lib/dbmw.h"
#include "lib/dbstore.h"
#include "lib/file.h"
#include "lib/glib-missing.h"
#include "lib/iprange.h"
#include "lib/halloc.h"
#include "lib/parse.h"
#include "lib/path.h"
#include "lib/random.h"
#include "lib/stringify.h"
#include "lib/tm.h"
#include "lib/walloc.h"
#include "lib/watcher.h"

#include "if/gnet_property.h"
#include "if/gnet_property_priv.h"
#include "if/bridge/c2ui.h"

#include "lib/override.h"		/* Must be the last header included */

typedef enum {
	HOSTILE_GLOBAL = 0,
	HOSTILE_PRIVATE = 1,

	NUM_HOSTILES
} hostiles_t;

static const char hostile[] = "hostile";
static const char hostiles_file[] = "hostiles.txt";
static const char * const hostiles_what[NUM_HOSTILES] = {
	"hostile IP addresses (global)",
	"hostile IP addresses (private)"
};

static struct iprange_db *hostile_db[NUM_HOSTILES];	/**< The hostile database */

/**
 * Hostile addresses dynamically collected at runtime for duration of a
 * session. If the hashtable reaches a certain size, we could create
 * a more efficient "iprange_db" from it.
 */
static hash_list_t *hl_dynamic_ipv4;

#define HOSTILES_DYNAMIC_PERIOD_MS	60161	/**< [ms]; about 1 minute (prime) */
#define HOSTILES_DYNAMIC_PENALTY	43201	/**< [s]; about 12 hours (prime) */

struct hostiles_dynamic_entry {
	guint32 ipv4;	/* MUST be at offset 0 due to uint32_hash/eq */
	unsigned long relative_time;
};

/**
 * DBM wrapper to associate an IP address with dynamically collected hostile
 * hosts that are returning known spam.
 */
static dbmw_t *db_spam;
static char db_spam_base[] = "spam_hosts";
static char db_spam_what[] = "Spamming hosts";

#define SPAM_MAX_PORTS			5		/**< Max amount of ports tracked */
#define SPAM_DB_CACHE_SIZE		512		/**< Amount of keys to keep in cache */
#define SPAM_DATA_VERSION		0		/**< Serialization version number */
#define SPAM_PRUNE_PERIOD		(3000 * 1000)	/**< in ms */
#define SPAM_SYNC_PERIOD		(60 * 1000)		/**< 1 minute, in ms */
#define SPAM_STABLE_PROBA		0.15	/**< 15% */
#define SPAM_STABLE_LIFETIME	(12 * 3600) 	/**< 12 hours */

/**
 * Information about a spamming servent.
 */
struct spamhost {
	time_t first_seen;		/**< Time first seen returning spam */
	time_t last_seen;		/**< Time last seen returning spam */
	guint16 port;			/**< Port number */
};

/**
 * Information about a spamming host that is stored to disk.
 * The structure is serialized first, not written as-is.
 *
 * The structure is keyed by its IP address.  It contains an array of at most
 * SPAM_MAX_PORTS entries, listing ports we have seen used by that host
 * for spamming purposes.  Ports are managed in an LRU fashion.
 */
struct spamdata {
	struct spamhost hosts[SPAM_MAX_PORTS];
	time_t create_time;		/**< When we first encountered that IP address */
	time_t last_time;		/**< Last time we saw spam from this host */
	guint8 ports;			/**< # of ports known to run spamming servents */
};

/**
 * Probabilities of allowing access to a host known to be spamming but for
 * which the port is a new one, given known "i" ports running spamming servents.
 * Probabilities are given as percentages in [0, 100].
 */
static unsigned spam_allow[SPAM_MAX_PORTS + 1] = { 100, 50, 20, 10, 5, 2 };

static cperiodic_t *hostiles_spam_prune_ev;		/**< Spamming host pruning */
static cperiodic_t *hostiles_spam_sync_ev;		/**< Spamming host DB sync */

/**
 * Serialization routine for spamdata.
 */
static void
serialize_spamdata(pmsg_t *mb, const void *data)
{
	const struct spamdata *sd = data;
	int i;

	g_assert(sd->ports <= SPAM_MAX_PORTS);

	pmsg_write_u8(mb, SPAM_DATA_VERSION);
	pmsg_write_time(mb, sd->create_time);
	pmsg_write_time(mb, sd->last_time);
	pmsg_write_u8(mb, sd->ports);

	for (i = 0; i < sd->ports; i++) {
		pmsg_write_be16(mb, sd->hosts[i].port);
		pmsg_write_time(mb, sd->hosts[i].first_seen);
		pmsg_write_time(mb, sd->hosts[i].last_seen);
	}
}

/**
 * Deserialization routine for spamdata.
 */
static void
deserialize_spamdata(bstr_t *bs, void *valptr, size_t len)
{
	struct spamdata *sd = valptr;
	guint8 version;
	int i;

	g_assert(sizeof *sd == len);

	bstr_read_u8(bs, &version);
	bstr_read_time(bs, &sd->create_time);
	bstr_read_time(bs, &sd->last_time);
	bstr_read_u8(bs, &sd->ports);

	sd->ports = MIN(sd->ports, SPAM_MAX_PORTS);

	for (i = 0; i < sd->ports; i++) {
		bstr_read_be16(bs, &sd->hosts[i].port);
		bstr_read_time(bs, &sd->hosts[i].first_seen);
		bstr_read_time(bs, &sd->hosts[i].last_seen);
	}
}

/**
 * Frees all entries in the given hostiles.
 */
static void
hostiles_close_one(hostiles_t which)
{
	guint i = which;
	
	g_assert(i < NUM_HOSTILES);
	iprange_free(&hostile_db[i]);
}

/**
 * Load hostile data from the supplied FILE.
 *
 * @returns the amount of entries loaded.
 */
static int
hostiles_load(FILE *f, hostiles_t which)
{
	char line[1024];
	guint32 ip, netmask;
	int linenum = 0;
	int bits;
	iprange_err_t error;

	g_assert(UNSIGNED(which) < NUM_HOSTILES);
	g_assert(NULL == hostile_db[which]);

	hostile_db[which] = iprange_new();

	while (fgets(line, sizeof line, f)) {
		linenum++;

		/*
		 * Remove all trailing spaces in string.
		 * Otherwise, lines which contain only spaces would cause a warning.
		 */

		if (!file_line_chomp_tail(line, sizeof line, NULL)) {
			g_warning("%s: line %d too long, aborting", G_STRFUNC, linenum);
			break;
		}

		if (file_line_is_skipable(line))
			continue;

		if (!string_to_ip_and_mask(line, &ip, &netmask)) {
			g_warning("%s, line %d: invalid IP or netmask \"%s\"",
				hostiles_file, linenum, line);
			continue;
		}

		bits = netmask_to_cidr(netmask);
		error = iprange_add_cidr(hostile_db[which], ip, bits,
					deconstify_gchar(hostile));

		switch (error) {
		case IPR_ERR_OK:
			break;
			/* FALL THROUGH */
		default:
			if (GNET_PROPERTY(reload_debug) || error != IPR_ERR_RANGE_SUBNET) {
				g_warning("%s, line %d: rejected entry \"%s\" (%s/%d): %s",
					hostiles_file, linenum, line, ip_to_string(ip), bits,
					iprange_strerror(error));
			}
			continue;
		}
	}

	iprange_sync(hostile_db[which]);

	if (GNET_PROPERTY(reload_debug)) {
		g_debug("loaded %u addresses/netmasks from %s (%u hosts)",
			iprange_get_item_count(hostile_db[which]), hostiles_what[which],
			iprange_get_host_count(hostile_db[which]));
	}
	return iprange_get_item_count(hostile_db[which]);
}

/**
 * Watcher callback, invoked when the file from which we read the hostile
 * addresses changed.
 */
static void
hostiles_changed(const char *filename, gpointer udata)
{
	FILE *f;
	char buf[80];
	int count;
	hostiles_t which;

	which = GPOINTER_TO_UINT(udata);
	g_assert(UNSIGNED(which) < NUM_HOSTILES);

	f = file_fopen(filename, "r");
	if (f == NULL)
		return;

	hostiles_close_one(which);
	count = hostiles_load(f, which);
	fclose(f);

	gm_snprintf(buf, sizeof(buf), "Reloaded %d hostile IP addresses.", count);
	gcu_statusbar_message(buf);

	node_kill_hostiles();
}

static void
hostiles_retrieve_from_file(FILE *f, hostiles_t which,
	const char *path, const char *filename)
{
	char *pathname;

	g_assert(f);
	g_assert(path);
	g_assert(filename);
	g_assert(UNSIGNED(which) < NUM_HOSTILES);

	pathname = make_pathname(path, filename);
	watcher_register(pathname, hostiles_changed, GUINT_TO_POINTER(which));
	HFREE_NULL(pathname);
	hostiles_load(f, which);
}

/**
 * Loads the hostiles.txt into memory.
 *
 * Choosing the first file we find among the several places we look at,
 * typically:
 *
 *	-# ~/.gtk-gnutella/hostiles.txt
 *	-# /usr/share/gtk-gnutella/hostiles.txt
 *	-# /home/src/gtk-gnutella/hostiles.txt
 *
 * The selected file will then be monitored and a reloading will occur
 * shortly after a modification.
 */
static G_GNUC_COLD void
hostiles_retrieve(hostiles_t which)
{
	g_assert(UNSIGNED(which) < NUM_HOSTILES);

	switch (which) {
	case HOSTILE_PRIVATE:
		{
			FILE *f;
			int idx;
			file_path_t fp_private[1];

			file_path_set(&fp_private[0], settings_config_dir(), hostiles_file);
			f = file_config_open_read_norename_chosen(
					hostiles_what[HOSTILE_PRIVATE],
					fp_private, G_N_ELEMENTS(fp_private), &idx);

			if (f) {
				hostiles_retrieve_from_file(f, HOSTILE_PRIVATE,
					fp_private[idx].dir, fp_private[idx].name);
				fclose(f);
			}
		}
		break;

	case HOSTILE_GLOBAL:
		{
			file_path_t fp[3];
			FILE *f;
			int idx;
			char *tmp;
			unsigned int length = 0;
			
#ifndef OFFICIAL_BUILD
			file_path_set(&fp[length++],
				PACKAGE_EXTRA_SOURCE_DIR, hostiles_file);
#endif
			file_path_set(&fp[length++], PRIVLIB_EXP, hostiles_file);

			tmp = get_folder_path(PRIVLIB_PATH, NULL);
			if (tmp != NULL)
				file_path_set(&fp[length++], tmp, hostiles_file);

			g_assert(length <= G_N_ELEMENTS(fp));

			f = file_config_open_read_norename_chosen(
					hostiles_what[HOSTILE_GLOBAL], fp, length, &idx);
					
			if (f) {
				hostiles_retrieve_from_file(f,
				HOSTILE_GLOBAL, fp[idx].dir, fp[idx].name);
				fclose(f);
			}
			HFREE_NULL(tmp);
		}
		break;

	case NUM_HOSTILES:
		g_assert_not_reached();
	}
}

/**
 * If the property was set to FALSE at startup time, hostile_db[HOSTILE_GLOBAL]
 * is still NULL and we need to load the global hostiles.txt now. Otherwise,
 * there's nothing to do, hostiles_check() will simply ignore
 * hostile_db[HOSTILE_GLOBAL]. The file watcher keeps running though during
 * this session and we keep the database in memory.
 */
static gboolean
use_global_hostiles_txt_changed(property_t unused_prop)
{
	(void) unused_prop;

	if (GNET_PROPERTY(use_global_hostiles_txt) && !hostile_db[HOSTILE_GLOBAL]) {
		hostiles_retrieve(HOSTILE_GLOBAL);
	}

    return FALSE;
}

static struct hostiles_dynamic_entry *
hostiles_dynamic_new(guint32 ipv4)
{
	struct hostiles_dynamic_entry entry;

	entry.ipv4 = ipv4;
	entry.relative_time = tm_relative_time();
	return wcopy(&entry, sizeof entry);
}

static void
hostiles_dynamic_free(struct hostiles_dynamic_entry **entry_ptr)
{
	struct hostiles_dynamic_entry *entry = *entry_ptr;

	if (entry) {
		wfree(entry, sizeof *entry);
		*entry_ptr = NULL;
	}
}

static void
hostiles_dynamic_expire(gboolean forced)
{
	unsigned long now = tm_relative_time();

	for (;;) {
		struct hostiles_dynamic_entry *entry;

		entry = hash_list_head(hl_dynamic_ipv4);
		if (NULL == entry)
			break;

		if (
			!forced &&
			delta_time(now, entry->relative_time) < HOSTILES_DYNAMIC_PENALTY
		)
			break;

		if (!forced && GNET_PROPERTY(ban_debug > 0)) {
			char buf[HOST_ADDR_BUFLEN];

			host_addr_to_string_buf(host_addr_get_ipv4(entry->ipv4),
				buf, sizeof buf);
			g_info("removing dynamically caught hostile: %s", buf);
		}
		hash_list_remove_head(hl_dynamic_ipv4);
		hostiles_dynamic_free(&entry);
		gnet_stats_count_general(GNR_SPAM_CAUGHT_HOSTILE_HELD, -1);
	}
}

/**
 * Callout queue periodic event to perform periodic monitoring of the
 * registered files.
 */
static gboolean
hostiles_dynamic_timer(void *unused_udata)
{
	(void) unused_udata;

	hostiles_dynamic_expire(FALSE);
	return TRUE;		/* Keep calling */
}

static void
hostiles_dynamic_add_ipv4(guint32 ipv4)
{
	struct hostiles_dynamic_entry *entry;

	if (hash_list_find(hl_dynamic_ipv4, &ipv4, cast_to_void_ptr(&entry))) {
		entry->relative_time = tm_relative_time();
		hash_list_moveto_tail(hl_dynamic_ipv4, entry);
	} else {
		entry = hostiles_dynamic_new(ipv4);
		hash_list_append(hl_dynamic_ipv4, entry);

		gnet_stats_count_general(GNR_SPAM_CAUGHT_HOSTILE_IP, 1);
		gnet_stats_count_general(GNR_SPAM_CAUGHT_HOSTILE_HELD, +1);
		if (GNET_PROPERTY(ban_debug > 0)) {
			char buf[HOST_ADDR_BUFLEN];

			host_addr_to_string_buf(host_addr_get_ipv4(ipv4), buf, sizeof buf);
			g_info("dynamically caught hostile: %s", buf);
		}
	}
}

static gboolean
hostiles_static_check_ipv4(guint32 ipv4)
{
	int i;

	for (i = 0; i < NUM_HOSTILES; i++) {
		if (i == HOSTILE_GLOBAL && !GNET_PROPERTY(use_global_hostiles_txt))
			continue;

		if (
			NULL != hostile_db[i] &&
			NULL != iprange_get(hostile_db[i], ipv4)
		)
			return TRUE;
	}
	return FALSE;
}

/**
 * Adds an IP address temporarily to the list of hostile addresses.
 * The address is forgotten when the process terminates.
 *
 * @note Only IPv4 addresses are handled, others are ignored.
 *		 While this could easily handle IPv6 addresses, too,
 *		 it must be considered that complete prefixes have to
 *		 banned because the list could quickly grow out of
 *		 proportion. In case of Teredo and 6to4 they're already
 *		 handled and indeed these equal IPv6 prefixes not just
 *		 individual addresses.
 *
 * @param addr 		the address to blacklist.
 * @param reason	how we detected that the address was hostile
 */
void
hostiles_dynamic_add(const host_addr_t addr, const char *reason)
{
	host_addr_t ipv4_addr;

	if (
		host_addr_convert(addr, &ipv4_addr, NET_TYPE_IPV4) ||
		host_addr_tunnel_client(addr, &ipv4_addr)
	) {
		guint32 ip = host_addr_ipv4(ipv4_addr);

		if (!hostiles_static_check_ipv4(ip)) {
			hostiles_dynamic_add_ipv4(ip);

			if (GNET_PROPERTY(spam_debug) > 1) {
				g_debug("SPAM dynamically caught hostile %s: %s",
					host_addr_to_string(ipv4_addr), reason);
			}
		}
	}
}

static inline gboolean
hostiles_dynamic_check_ipv4(guint32 ipv4)
{
	/**
	 * We could check relative_time here but entries are sufficiently
	 * frequently expired and the timeout is arbitrary anyway.
	 */
	return hash_list_contains(hl_dynamic_ipv4, &ipv4);
}

/**
 * Check the given address against the entries in the hostiles.
 *
 * @param ha the host address to check.
 * @returns TRUE if found, and FALSE if not.
 */
gboolean
hostiles_check(const host_addr_t ha)
{
	host_addr_t to;

	if (
		host_addr_convert(ha, &to, NET_TYPE_IPV4) ||
		host_addr_tunnel_client(ha, &to)
	) {
		guint32 ip = host_addr_ipv4(to);

		return hostiles_dynamic_check_ipv4(ip) ||
			hostiles_static_check_ipv4(ip);
	}

	return FALSE;
}

/**
 * Get spamdata from database, returning NULL if not found.
 */
static struct spamdata *
get_spamdata(const gnet_host_t *host)
{
	struct spamdata *sd;

	sd = dbmw_read(db_spam, host, NULL);

	if (NULL == sd) {
		if (dbmw_has_ioerr(db_spam)) {
			g_warning("DBMW \"%s\" I/O error", dbmw_name(db_spam));
		}
	}

	return sd;
}

/**
 * Record indication that we got spam from given address and port.
 */
void
hostiles_spam_add(const host_addr_t addr, guint16 port)
{
	struct spamdata *sd;
	struct spamdata new_sd;
	gnet_host_t host;

	/*
	 * For convenience reasons, our keys are gnet_host_t objects but we
	 * don't use the port number in the key, so we set it to 0 here.
	 */

	gnet_host_set(&host, addr, 0);
	sd = get_spamdata(&host);

	if (NULL == sd) {
		sd = &new_sd;
		sd->ports = 1;
		sd->hosts[0].first_seen = sd->hosts[0].last_seen =
			sd->create_time = sd->last_time = tm_time();
		sd->hosts[0].port = port;
		gnet_stats_count_general(GNR_SPAM_IP_HELD, +1);
	} else {
		int i;
		gboolean found = FALSE;

		for (i = 0; i < sd->ports; i++) {
			struct spamhost *sh = &sd->hosts[i];

			if (sh->port == port) {
				sh->last_seen = tm_time();
				found = TRUE;
				break;
			}
		}

		if (!found) {
			int slot;
			struct spamhost *sh;

			if (SPAM_MAX_PORTS == sd->ports) {
				time_t oldest = MAX_INT_VAL(time_t);

				/*
				 * Array is full, find the least recently seen port.
				 */

				for (i = 0, slot = -1; i < sd->ports; i++) {
					sh = &sd->hosts[i];

					if (sh->last_seen < oldest) {
						oldest = sh->last_seen;
						slot = i;
					}
				}

				g_assert(slot >= 0 && slot < SPAM_MAX_PORTS);

				if (GNET_PROPERTY(spam_debug) > 5) {
					g_debug("SPAM discarding port %u for host %s",
						sd->hosts[slot].port, host_addr_to_string(addr));
				}
			} else {
				g_assert(sd->ports < SPAM_MAX_PORTS);
				slot = sd->ports++;
			}

			/*
			 * Fill selected slot.
			 */

			sh = &sd->hosts[slot];
			sh->port = port;
			sh->first_seen = sh->last_seen = tm_time();
		}

		sd->last_time = tm_time();
	}

	dbmw_write(db_spam, &host, sd, sizeof *sd);
}

/**
 * Remove the given port entry from the spamdata structure and commit
 * change to database.
 */
static void
spam_remove_port(struct spamdata *sd, const host_addr_t addr, guint16 port)
{
	int i;

	g_assert(sd != NULL);

	for (i = 0; i < sd->ports; i++) {
		struct spamhost *sh = &sd->hosts[i];

		if (port == sh->port) {
			gnet_host_t host;

			sd->ports--;
			if (i < sd->ports) {
				memmove(&sd->hosts[i], &sd->hosts[i+1],
					sizeof(sd->hosts[0]) * (sd->ports - i));
			}
			if (GNET_PROPERTY(spam_debug) > 5) {
				g_debug("SPAM removing port %u for host %s (%u port%s remain)",
					port, host_addr_to_string(addr), sd->ports,
					1 == sd->ports ? "" : "s");
			}
			gnet_host_set(&host, addr, 0);
			dbmw_write(db_spam, &host, sd, sizeof *sd);
			break;
		}
	}
}

/**
 * Is IP:port that of a known host returning spam?
 */
gboolean
hostiles_spam_check(const host_addr_t addr, guint16 port)
{
	struct spamdata *sd;
	gnet_host_t host;
	int i;
	unsigned c;

	/*
	 * For convenience reasons, our keys are gnet_host_t objects but we
	 * don't use the port number in the key, so we set it to 0 here.
	 */

	gnet_host_set(&host, addr, 0);
	sd = get_spamdata(&host);

	if (NULL == sd)
		return FALSE;

	/*
	 * Look whether we get an exact match for the port.
	 */

	g_assert(sd->ports <= SPAM_MAX_PORTS);

	for (i = 0; i < sd->ports; i++) {
		struct spamhost *sh = &sd->hosts[i];
		gboolean expired;

		if (sh->port != port)
			continue;

		/*
		 * Make sure this IP:port has not expired, using our probability model.
		 *
		 * The reason we keep track of ports on a per-IP level (as opposed to
		 * just tracking IP addresses) is because the IP could be assigned
		 * to a given endpoint on a temporary basis.  When the IP is reassigned
		 * to someone else, the chances that the same port be used are slim,
		 * provided people don't use the standard ports.
		 *
		 * We do not keep entries on an IP:port basis to be able to see whether
		 * spam comes from several ports on the same IP address, which would
		 * tend to indicate a spamming farm, with hosts running multiple
		 * spamming servents on each IP address.
		 */

		if (sh->first_seen == sh->last_seen) {
			expired = delta_time(tm_time(), sh->last_seen) >
				SPAM_STABLE_LIFETIME;
		} else {
			double p;
			p = stable_still_alive_probability(sh->first_seen, sh->last_seen);
			expired = p < SPAM_STABLE_PROBA;
		}

		if (expired) {
			spam_remove_port(sd, addr, port);
			break;
		} else {
			return TRUE;		/* We have a match on IP and port */
		}
	}

	/*
	 * If we can contact the host, then it's not a spamming host.
	 *
	 * We had no real port matching, so the probability depends on the
	 * amount of ports that are already known to issue spam on the host.
	 */

	c = spam_allow[sd->ports];

	if (100 == c) {
		return FALSE;			/* Not a spamming host */
	} else {
		return random_u32() % 100 >= c;
	}
}

/**
 * DBMW foreach iterator to remove old entries.
 * @return TRUE if entry must be deleted.
 */
static gboolean
spam_prune_old(void *key, void *value, size_t u_len, void *u_data)
{
	const gnet_host_t *h = key;
	const struct spamdata *sd = value;
	time_delta_t d;
	double p = 0.0;
	gboolean expired;

	(void) u_len;
	(void) u_data;

	/*
	 * First look whether the overall host entry has expired.
	 *
	 * We reuse the statistical probability model of DHT nodes to project
	 * whether it makes sense to keep an entry.
	 *
	 * Since by construction we do not contact these hosts very often, the
	 * stable probability is set low.  To compensate for that, we also add
	 * a failsafe by limiting the amount of time we keep it in the cache
	 * after last seeing it if the create time and the last seen time are
	 * identical.
	 */

	d = delta_time(tm_time(), sd->last_time);
	if (sd->create_time == sd->last_time) {
		expired = d > SPAM_STABLE_LIFETIME;
	} else {
		p = stable_still_alive_probability(sd->create_time, sd->last_time);
		expired = p < SPAM_STABLE_PROBA;
	}

	if (GNET_PROPERTY(spam_debug) > 5) {
		g_debug("SPAM cached %s life=%s last_seen=%s, p=%.2f%%%s",
			host_addr_to_string(gnet_host_get_addr(h)),
			compact_time(delta_time(sd->last_time, sd->create_time)),
			compact_time2(d), p * 100.0,
			expired ? " [EXPIRED]" : "");
	}

	return expired;
}

/**
 * Prune the database, removing expired hosts.
 */
static void
hostiles_spam_prune_old(void)
{
	if (GNET_PROPERTY(spam_debug)) {
		g_debug("SPAM pruning expired hosts (%zu)", dbmw_count(db_spam));
	}

	dbmw_foreach_remove(db_spam, spam_prune_old, NULL);
	gnet_stats_set_general(GNR_SPAM_IP_HELD, dbmw_count(db_spam));

	if (GNET_PROPERTY(spam_debug)) {
		g_debug("SPAM pruned expired hosts (%zu remaining)",
			dbmw_count(db_spam));
	}
}

/**
 * Callout queue periodic event to expire old entries.
 */
static gboolean
hostiles_spam_periodic_prune(void *unused_obj)
{
	(void) unused_obj;

	hostiles_spam_prune_old();
	return TRUE;		/* Keep calling */
}

/**
 * Callout queue periodic event to synchronize the disk image.
 */
static gboolean
hostiles_spam_periodic_sync(void *unused_obj)
{
	(void) unused_obj;

	dbstore_sync_flush(db_spam);
	return TRUE;		/* Keep calling */
}

/**
 * Called on startup. Loads the hostiles.txt into memory.
 */
G_GNUC_COLD void
hostiles_init(void)
{
	dbstore_kv_t kv =
		{ sizeof(gnet_host_t), gnet_host_length, sizeof(struct spamdata), 0 };
	dbstore_packing_t packing =
		{ serialize_spamdata, deserialize_spamdata, NULL };

	g_assert(NULL == db_spam);

	db_spam = dbstore_open(db_spam_what, settings_gnet_db_dir(),
		db_spam_base, kv, packing, SPAM_DB_CACHE_SIZE,
		gnet_host_hash, gnet_host_eq, FALSE);

	hostiles_spam_prune_old();

	hostiles_spam_prune_ev = cq_periodic_main_add(
		SPAM_PRUNE_PERIOD, hostiles_spam_periodic_prune, NULL);
	hostiles_spam_sync_ev = cq_periodic_main_add(
		SPAM_SYNC_PERIOD, hostiles_spam_periodic_sync, NULL);

	hl_dynamic_ipv4 = hash_list_new(uint32_hash, uint32_eq);
	cq_periodic_main_add(
		HOSTILES_DYNAMIC_PERIOD_MS, hostiles_dynamic_timer, NULL);
	hostiles_retrieve(HOSTILE_PRIVATE);
    gnet_prop_add_prop_changed_listener(PROP_USE_GLOBAL_HOSTILES_TXT,
		use_global_hostiles_txt_changed, TRUE);
}

/**
 * Frees all entries in all the hostiles.
 */
void
hostiles_close(void)
{
	int i;

	for (i = 0; i < NUM_HOSTILES; i++) {
		hostiles_close_one(i);
	}

	gnet_prop_remove_prop_changed_listener(PROP_USE_GLOBAL_HOSTILES_TXT,
		use_global_hostiles_txt_changed);
	hostiles_dynamic_expire(TRUE);
	hash_list_free(&hl_dynamic_ipv4);

	dbstore_close(db_spam, settings_gnet_db_dir(), db_spam_base);
	db_spam = NULL;
	cq_periodic_remove(&hostiles_spam_prune_ev);
	cq_periodic_remove(&hostiles_spam_sync_ev);
}

/* vi: set ts=4 sw=4 cindent: */
