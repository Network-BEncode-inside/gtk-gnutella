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
 * @ingroup core
 * @file
 *
 * Banning control.
 *
 * @author Raphael Manfredi
 * @date 2002-2003
 */

#include "common.h"

RCSID("$Id$")

#include "ban.h"
#include "sockets.h"		/* For socket_register_fd_reclaimer() */
#include "token.h"
#include "whitelist.h"

#include "lib/atoms.h"
#include "lib/fd.h"
#include "lib/file.h"		/* For file_register_fd_reclaimer() */
#include "lib/cq.h"
#include "lib/misc.h"
#include "lib/tm.h"
#include "lib/walloc.h"

#include "if/gnet_property.h"
#include "if/gnet_property_priv.h"

#include "lib/override.h"	/* Must be the last header included */

/*
 * We keep a hash table, indexed by IP address, which records all the
 * requests we have from the various IPs.  When hammering is detected,
 * the IP address is banned for some time.
 *
 * We use linear decay to gradually decrease the amount of requests made
 * over time.
 */

#define BAN_DELAY		300		/**< Initial ban delay: 5 minutes */
#define MAX_REQUEST		5		/**< Maximum of 5 requests... */
#define MAX_PERIOD		60		/**< ...per minute */
#define MAX_BAN			10800	/**< 3 hours */
#define BAN_REMIND		5		/**< Every so many attemps, tell them about it */
#define BAN_CALLOUT		1000	/**< Every 1 second */

#define ban_reason(p)	((p)->ban_msg ? (p)->ban_msg : "N/A")

static GHashTable *info;		/**< Info by IP address */
static cqueue_t *ban_cq;		/**< Private callout queue */

/**< Decay coefficient, per second */
static const float decay_coeff = (float) MAX_REQUEST / MAX_PERIOD;

/***
 *** Hammering-specific banning.
 ***/

/**
 * Information kept in the info table, per IP address.
 */
struct addr_info {
	float counter;				/**< Counts connection, decayed linearily */
	host_addr_t addr;			/**< IP address */
	time_t created;				/**< When did last connection occur? */
	cevent_t *cq_ev;			/**< Scheduled callout event */
	int ban_delay;				/**< Banning delay, in seconds */
	int ban_count;				/**< Amount of time we banned this source */
	const char *ban_msg;		/**< Banning message (atom) */
	gboolean banned;			/**< Is this IP currently banned? */
};

static void ipf_destroy(cqueue_t *cq, gpointer obj);

/**
 * Create new addr_info structure for said IP.
 */
static struct addr_info *
ipf_make(const host_addr_t addr, time_t now)
{
	struct addr_info *ipf;

	WALLOC(ipf);

	ipf->counter = 1.0;
	ipf->addr = addr;
	ipf->created = now;
	ipf->ban_delay = 0;
	ipf->ban_count = 0;
	ipf->ban_msg = NULL;
	ipf->banned = FALSE;

	/*
	 * Schedule collecting of record.
	 *
	 * Our counter is 1, and the liner decay per second is decay_coeff,
	 * so it will reach 0 in 1/decay_coeff seconds.  The callout queue takes
	 * time in milli-seconds.
	 */
	{
		int delay;
		
		delay = 1000.0 / decay_coeff;
		delay = MAX(delay, 1);
		ipf->cq_ev = cq_insert(ban_cq, delay, ipf_destroy, ipf);
	}

	return ipf;
}

/**
 * Free addr_info structure.
 */
static void
ipf_free(struct addr_info *ipf)
{
	g_assert(ipf);

	cq_cancel(&ipf->cq_ev);
	atom_str_free_null(&ipf->ban_msg);
	WFREE(ipf);
}

/**
 * Called from callout queue when it's time to destroy the record.
 */
static void
ipf_destroy(cqueue_t *unused_cq, gpointer obj)
{
	struct addr_info *ipf = obj;

	(void) unused_cq;
	g_assert(ipf);
	g_assert(!ipf->banned);
	g_assert(ipf == g_hash_table_lookup(info, &ipf->addr));

	if (GNET_PROPERTY(ban_debug) > 8)
		g_debug("disposing of BAN %s: %s",
			host_addr_to_string(ipf->addr), ban_reason(ipf));

	g_hash_table_remove(info, &ipf->addr);
	ipf->cq_ev = NULL;
	ipf_free(ipf);
}

/**
 * Called from callout queue when it's time to unban the IP.
 */
static void
ipf_unban(cqueue_t *unused_cq, gpointer obj)
{
	struct addr_info *ipf = obj;
	time_t now = tm_time();
	int delay;

	(void) unused_cq;
	g_assert(ipf);
	g_assert(ipf->banned);
	g_assert(ipf == g_hash_table_lookup(info, &ipf->addr));

	/*
	 * Decay counter by measuring the amount of seconds since last connection
	 * and applying the linear decay coefficient.
	 */

	ipf->counter -= delta_time(now, ipf->created) * decay_coeff;
	ipf->created = now;

	if (GNET_PROPERTY(ban_debug) > 2)
		g_debug("lifting BAN for %s (%s), counter = %.3f",
			host_addr_to_string(ipf->addr), ban_reason(ipf), ipf->counter);

	/**
	 * Compute new scheduling delay.
	 */

	delay = 1000.0 * ipf->counter / decay_coeff;

	/**
	 * If counter is negative or null, we can remove the entry.
	 * Since we round to an integer, we must consider `delay' and
	 * not the original counter.
	 */

	if (delay <= 0) {
		if (GNET_PROPERTY(ban_debug) > 8)
			g_debug("disposing of BAN %s: %s",
				host_addr_to_string(ipf->addr), ban_reason(ipf));

		g_hash_table_remove(info, &ipf->addr);
		ipf->cq_ev = NULL;
		ipf_free(ipf);
		return;
	}

	ipf->banned = FALSE;
	atom_str_free_null(&ipf->ban_msg);
	ipf->cq_ev = cq_insert(ban_cq, delay, ipf_destroy, ipf);
}

/**
 * Check whether we can allow connection from `ip' to proceed.
 *
 * Returns:
 *
 *   BAN_OK     ok, can proceed with connection.
 *   BAN_FIRST  will ban, but send back message, then close connection.
 *   BAN_FORCE	don't send back anything, and call ban_force().
 *   BAN_MSG	will ban with explicit message and tailored error code.
 */
ban_type_t
ban_allow(const host_addr_t addr)
{
	struct addr_info *ipf;
	time_t now = tm_time();

	switch (host_addr_net(addr)) {
	case NET_TYPE_IPV4:
	case NET_TYPE_IPV6:
		break;
	default:
		return BAN_OK;
	}

	if (whitelist_check(addr))
		return BAN_OK;

	ipf = g_hash_table_lookup(info, &addr);

	/*
	 * First time we see this IP?  It's OK then.
	 */

	if (NULL == ipf) {
		ipf = ipf_make(addr, now);
		g_hash_table_insert(info, &ipf->addr, ipf);
		return BAN_OK;
	}

	/*
	 * Decay counter by measuring the amount of seconds since last connection
	 * and applying the linear decay coefficient.
	 */

	ipf->counter -= delta_time(now, ipf->created) * decay_coeff;

	if (ipf->counter < 0.0)
		ipf->counter = 0.0;

	/*
	 * Account for the new connection.
	 *
	 * Note that connections made during the ban time are also accounted for,
	 * which will possibly penalize the remote IP when it is unbanned!
	 */

	ipf->counter += 1.0;
	ipf->created = now;

	if (GNET_PROPERTY(ban_debug) > 4)
		g_debug("BAN %s, counter = %.3f (%s)",
			host_addr_to_string(ipf->addr), ipf->counter,
			ipf->banned ? "already banned" :
			ipf->counter > (float) MAX_REQUEST ? "banning" : "OK");

	g_assert(ipf->cq_ev);

	/*
	 * If the IP is already banned, it already has an "unban" callback.
	 *
	 * When there is a message recorded, return BAN_MSG to signal that
	 * we need special processing: dedicated error code, and message to
	 * extract.
	 */

	if (ipf->banned) {
		if (ipf->ban_msg != NULL)
			return BAN_MSG;

		/**
		 * Every BAN_REMIND attempts, return BAN_FIRST to let them know
		 * that they have been banned, in case they "missed" our previous
		 * indications or did not get the Retry-After right.
		 *		--RAM, 2004-06-21
		 */

		if (++(ipf->ban_count) % BAN_REMIND == 0)
			return BAN_FIRST;

		return BAN_FORCE;
	}

	/*
	 * Ban the IP if it crossed the request limit.
	 */

	if (ipf->counter > (float) MAX_REQUEST) {
		cq_cancel(&ipf->cq_ev);		/* Cancel ipf_destroy */

		ipf->banned = TRUE;
		atom_str_change(&ipf->ban_msg, "Too frequent connections");

		if (ipf->ban_delay)
			ipf->ban_delay *= 2;
		else
			ipf->ban_delay = BAN_DELAY;

		if (ipf->ban_delay > MAX_BAN)
			ipf->ban_delay = MAX_BAN;

		ipf->cq_ev = cq_insert(ban_cq, 1000 * ipf->ban_delay, ipf_unban, ipf);

		return BAN_FIRST;
	}

	/*
	 * OK, we accept this connection.  Reschedule cleanup.
	 */
	{
		int delay;

		delay = 1000.0 * ipf->counter / decay_coeff;
		delay = MAX(delay, 1);
		cq_resched(ipf->cq_ev, delay);
	}

	return BAN_OK;
}

/**
 * Record banning with specific message for a given IP, for MAX_BAN seconds.
 */
void
ban_record(const host_addr_t addr, const char *msg)
{
	struct addr_info *ipf;

	/*
	 * If is possible that we already have an addr_info for that host.
	 */

	ipf = g_hash_table_lookup(info, &addr);

	if (NULL == ipf) {
		ipf = ipf_make(addr, tm_time());
		g_hash_table_insert(info, &ipf->addr, ipf);
	}

	atom_str_change(&ipf->ban_msg, msg);
	ipf->ban_delay = MAX_BAN;

	if (GNET_PROPERTY(ban_debug))
		g_debug("BAN %s record %s: %s",
			ipf->banned ? "updating" : "new",
			host_addr_to_string(ipf->addr), ban_reason(ipf));

	if (ipf->banned)
		cq_resched(ipf->cq_ev, MAX_BAN * 1000);
	else {
		cq_cancel(&ipf->cq_ev);		/* Cancel ipf_destroy */
		ipf->banned = TRUE;
		ipf->cq_ev = cq_insert(ban_cq, MAX_BAN * 1000, ipf_unban, ipf);
	}
}

/*
 * Banning structures.
 *
 * We maintain a FIFO of all the file descriptors we've banned.  When we
 * have `max_banned_fd' entries in the FIFO, start closing the oldest one.
 */

#define SOCK_BUFFER		512				/**< Reduced socket buffer */

static GList *banned_head = NULL;
static GList *banned_tail = NULL;

static void
ban_close_fd(void **data_ptr)
{
	void *data = *data_ptr;
	int fd = GPOINTER_TO_INT(data);

	g_assert(is_valid_fd(fd));
	g_assert(fd > STDERR_FILENO);	/* fd 0-2 are not used for sockets */

	if (GNET_PROPERTY(ban_debug) > 9) {
		g_debug("closing BAN fd #%d", fd);
	}
	fd_close(&fd);	/* Reclaim fd */
	*data_ptr = GINT_TO_POINTER(-1);
}

/**
 * Internal version of ban_reclaim_fd().
 *
 * Reclaim a file descriptor used for banning.
 *
 * @returns TRUE if we did reclaim something, FALSE if there was nothing.
 */
static gboolean
reclaim_fd(void)
{
	GList *prev;

	if (banned_tail == NULL) {
		g_assert(banned_head == NULL);
		g_assert(GNET_PROPERTY(banned_count) == 0);
		return FALSE;					/* Empty list */
	}

	g_assert(banned_head != NULL);
	g_assert(GNET_PROPERTY(banned_count) > 0);

	ban_close_fd(&banned_tail->data);

	prev = g_list_previous(banned_tail);
	banned_head = g_list_remove_link(banned_head, banned_tail);
	g_list_free_1(banned_tail);
	banned_tail = prev;

	gnet_prop_decr_guint32(PROP_BANNED_COUNT);

	return TRUE;
}

/**
 * Reclaim a file descriptor used for banning
 *
 * Invoked from the outside as a callback to reclaim file descriptors.
 *
 * This routine is called when there is a shortage of file descriptors, so
 * we activate the "file_descriptor_shortage" property.  However, if we have
 * nothing to reclaim, we activate the "file_descriptor_runout" property
 * instead, which signifies that processing will be degraded.
 *
 * @returns TRUE if we did reclaim something, FALSE if there was nothing.
 */
static gboolean
ban_reclaim_fd(void)
{
	gboolean reclaimed;

	reclaimed = reclaim_fd();

	/*
	 * Those properties will be cleared if more than 10 minutes elapse
	 * after their last setting to TRUE.
	 */

	if (reclaimed)
		gnet_prop_set_boolean_val(PROP_FILE_DESCRIPTOR_SHORTAGE, TRUE);
	else
		gnet_prop_set_boolean_val(PROP_FILE_DESCRIPTOR_RUNOUT, TRUE);

	return reclaimed;
}

/**
 * Force banning of the connection.
 *
 * We're putting it in a list and forgetting about it.
 */
void
ban_force(struct gnutella_socket *s)
{
	int fd;

	socket_check(s);
	fd = s->file_desc;
	g_return_if_fail(is_valid_fd(fd));
	g_return_if_fail(fd > STDERR_FILENO); /* fd 0-2 are not used for sockets */

	if (GNET_PROPERTY(banned_count) >= GNET_PROPERTY(max_banned_fd)) {
		g_assert(banned_tail);
		g_assert(GNET_PROPERTY(max_banned_fd) <= 1 ||
			banned_tail != banned_head);

		reclaim_fd();
	}

	/* Ensure we're not listening to I/O events anymore. */
	socket_evt_clear(s);

	/*
	 * Shrink socket buffers.
	 */

	socket_send_buf(s, SOCK_BUFFER, TRUE);
	socket_recv_buf(s, SOCK_BUFFER, TRUE);

	/*
	 * Let the kernel discard incoming data; SHUT_WR or SHUT_RDWR
	 * would cause to sent a FIN which we want to prevent.
	 */
	shutdown(s->file_desc, SHUT_RD);

	s->file_desc = -1;				/* Prevent fd close by socket_free() */

	/*
	 * Insert banned fd in the list.
	 */

	banned_head = g_list_prepend(banned_head, GINT_TO_POINTER(fd));
	if (banned_tail == NULL)
		banned_tail = banned_head;

	gnet_prop_incr_guint32(PROP_BANNED_COUNT);
}

/**
 * Check whether IP is already recorded as being banned.
 */
gboolean
ban_is_banned(const host_addr_t addr)
{
	struct addr_info *ipf;

	ipf = g_hash_table_lookup(info, &addr);

	return ipf != NULL && ipf->banned;
}

/**
 * @return banning delay for banned IP.
 */
int
ban_delay(const host_addr_t addr)
{
	struct addr_info *ipf;

	ipf = g_hash_table_lookup(info, &addr);
	g_assert(ipf);

	return ipf->ban_delay;
}

/**
 * @return banning message for banned IP.
 */
const char *
ban_message(const host_addr_t addr)
{
	struct addr_info *ipf;

	ipf = g_hash_table_lookup(info, &addr);
	g_assert(ipf);

	return ipf->ban_msg;
}

/**
 * Initialize the banning system.
 */
void
ban_init(void)
{
	info = g_hash_table_new(host_addr_hash_func, host_addr_eq_func);
	ban_cq = cq_submake("ban", callout_queue, BAN_CALLOUT);

	ban_max_recompute();
	file_register_fd_reclaimer(ban_reclaim_fd);
	socket_register_fd_reclaimer(ban_reclaim_fd);
}

/**
 * Recompute the maximum amount of file descriptors we dedicate to banning.
 */
void
ban_max_recompute(void)
{
	guint32 max;

	max = (GNET_PROPERTY(sys_nofile) * GNET_PROPERTY(ban_ratio_fds)) / 100;
	max = MIN(GNET_PROPERTY(ban_max_fds), max);
	max = MAX(1, max);

	if (GNET_PROPERTY(ban_debug))
		g_info("will use at most %d file descriptor%s for banning",
			max, max == 1 ? "" : "s");

	gnet_prop_set_guint32_val(PROP_MAX_BANNED_FD, max);
}

static void
free_info(gpointer unused_key, gpointer value, gpointer unused_udata)
{
	(void) unused_key;
	(void) unused_udata;
	ipf_free(value);
}

/**
 * Called at shutdown time to reclaim all memory.
 */
void
ban_close(void)
{
	GList *l;

	g_hash_table_foreach(info, free_info, NULL);
	gm_hash_table_destroy_null(&info);

	for (l = banned_head; NULL != l; l = g_list_next(l)) {
		ban_close_fd(&l->data); /* Reclaim fd */
	}

	gm_list_free_null(&banned_head);
	cq_free_null(&ban_cq);
}

/***
 *** Vendor-specific banning.
 ***/

/*
 * These messages are sent to the remote site. Don't localize them.
 */
static const char harmful[] = "Harmful version banned, upgrade required";
static const char refused[] = "Connection refused";

/**
 * Check whether servent identified by its vendor string should be banned.
 * When we ban, we ban for both gnet and download connections.  Such banning
 * is exceptional, usually restricted to some versions and the servent's author
 * is informed about the banning.
 *
 * @returns NULL if we shall not ban, a banning reason string otherwise.
 */
const char *
ban_vendor(const char *vendor)
{
	const char *gtkg_version;

	/*
	 * If vendor starts with "!gtk-gnutella", skip the leading '!' for
	 * our tests here.
	 */

	if (vendor[0] == '!') {
		if (NULL != (gtkg_version = is_strprefix(&vendor[1], "gtk-gnutella/")))
			vendor++;
	} else {
		gtkg_version = is_strprefix(vendor, "gtk-gnutella/");
	}

	/*
	 * Ban gtk-gnutella/0.90 from the network.  This servent had
	 * bugs that could corrupt the traffic.  Also ban 0.91u.
	 *
	 * Versions of GKTG deemed too old are also banned: the Gnutella
	 * network is far from being mature, and we need to ensure newer
	 * features are deployed reasonably quickly.
	 *		--RAM, 03/01/2002.
	 */

	if (gtkg_version) {
		static const char * const versions[] = {
			"0.90",
			"0.91u",
			"0.92b",
			"0.93",
			"0.94",
		};
		guint i;

		for (i = 0; i < G_N_ELEMENTS(versions); i++) {
			if (is_strprefix(gtkg_version, versions[i]))
				return harmful;
		}

		return NULL;
	}

	if (vendor[0] == 'G') {
		const char *ver;

		if (NULL != (ver = is_strprefix(vendor, "Gnucleus "))) {
			if (is_strprefix(ver, "1.6.0.0"))
				return harmful;
		} else if (is_strprefix(vendor, "Gtk-Gnutella "))
			return refused;

		return NULL;
	}

	return NULL;
}

/* vi: set ts=4 sw=4 cindent: */
