/*
 * $Id$
 *
 * Copyright (c) 2004, Thomas Schuerger & Jeroen Asselman
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
 * Horizon Size Estimation Protocol 0.2.
 *
 * Protocol is defined here: http://www.menden.org/gnutella/hsep.html
 *
 * @author Thomas Schuerger
 * @author Jeroen Asselman
 * @date 2004
 *
 * General API information:
 *
 * - hsep_init() should be called once on startup of GtkG
 * - hsep_connection_init(node) should be called once for each
 *   newly established HSEP-capable connection
 * - hsep_connection_close(node) should be called when a HSEP-capable
 *   connection is closed
 * - hsep_timer() should be called frequently to send out
 *   HSEP messages to HSEP-capable nodes as required
 * - hsep_notify_shared(files, kibibytes) should be called whenever the
 *   number of shared files and/or kibibytes has changed
 * - hsep_process_msg(node) should be called whenever a HSEP message
 *   is received from a HSEP-capable node
 * - hsep_reset() can be used to reset all HSEP data (not for normal use)
 * - hsep_get_global_table(dest, triples) can be used to get the global
 *   HSEP table
 * - hsep_get_connection_table(conn, dest, triples) can be used to get a
 *   per-connection HSEP table
 * - hsep_add_global_table_listener(cb, freqtype, interval) can be used to
 *   add a listener that is informed whenever the global HSEP table changes.
 * - hsep_remove_global_table_listener(cb) can be used to remove an added
 *   listener for global HSEP table changes.
 * - hsep_has_global_table_changed(since) can be used to check if the
 *   global HSEP table has changed since the specified point in time.
 * - hsep_get_non_hsep_triple(tripledest) can be used to determine the
 *   reachable resources contributed by non-HSEP nodes (this is what direct
 *   neighbors that don't support HSEP tell us they're sharing).
 *
 * Obtaining horizon size information on demand:
 *
 * To obtain horizon size information, use the global HSEP table or the
 * per-connection HSEP table, obtained using hsep_get_global_table(...) or
 * hsep_get_connection_table (...), respectively (never access the internal
 * arrays directly). To check if the global table has changed, use
 * hsep_has_global_table_changed(...). The usable array indexes are between 1
 * (for 1 hop) and HSEP_N_MAX (for n_max hops). Note that the arrays only
 * consider other nodes (i.e. exclude what we share ourselves), so the array
 * index 0 always contains zeros. Note also that each triple represents the
 * reachable resources *within* the number of hops, not at *exactly* the number
 * of hops. To get the values for exactly the number of hops, simply subtract
 * the preceeding triple from the desired triple.
 *
 * Obtaining horizon size information using event-driven callbacks (only
 * for the global HSEP table):
 *
 * You can register a callback function for being informed whenever the
 * global HSEP table changes by calling hsep_add_global_table_listener(...).
 * On change of the global HSEP table the callback will be called with a pointer
 * to a copy of the HSEP table and the number of provided triples. You must
 * remove the listener later using hsep_remove_global_table_listener(...).
 *
 * @note
 * To support exchanging information about clients that don't support
 * HSEP, these clients' library sizes (from PONG messages) are taken into
 * account when HSEP messages are sent (that info is added to what we see
 * in a distance of >= 1 hop).
 */

#include "common.h"

RCSID("$Id$")

#include "gmsg.h"
#include "routing.h"
#include "nodes.h"
#include "hsep.h"
#include "uploads.h"
#include "share.h"
#include "features.h"

#include "if/gnet_property.h"
#include "if/gnet_property_priv.h"

#include "lib/endian.h"
#include "lib/glib-missing.h"
#include "lib/tm.h"
#include "lib/walloc.h"
#include "lib/override.h"

/** global HSEP table */
static hsep_triple hsep_global_table[HSEP_N_MAX + 1];

/*
 * My own HSEP triple (first value must not be changed, the other must be
 * be updated whenever the number of our shared files/kibibytes change
 * by calling hsep_notify_shared()).
 */

static hsep_triple hsep_own = {1, 0, 0};

static event_t *hsep_global_table_changed_event;
static time_t hsep_last_global_table_change = 0;

/**
 * Fires a change event for the global HSEP table.
 */

static void
hsep_fire_global_table_changed(time_t now)
{
	/* store global table change time */
	hsep_last_global_table_change = now;

	/* do nothing if we don't have any listeners */

	if (event_subscriber_active(hsep_global_table_changed_event)) {
		hsep_triple table[G_N_ELEMENTS(hsep_global_table)];

		/*
		 * Make a copy of the global HSEP table and give that
		 * copy and the number of included triples to the
		 * listeners.
		 */

		hsep_get_global_table(table, G_N_ELEMENTS(table));

		event_trigger(hsep_global_table_changed_event,
		    T_NORMAL(hsep_global_listener_t, (table, G_N_ELEMENTS(table))));
	}
}

/**
 * Checks the monotony of the given triples. TRUE is returned if 0 or 1
 * triple is given. Returns TRUE if monotony is ok, FALSE otherwise.
 */

static gboolean
hsep_check_monotony(hsep_triple *table, unsigned int triples)
{
	gboolean error = FALSE;
	guint i, j;

	g_assert(table);

	for (i = 1; i < triples; i++) {

		/* if any triple is not >= the previous one, error will be TRUE */
		for (j = 0; j < G_N_ELEMENTS(table[0]); j++)
			error |= table[i - 1][j] > table[i][j];

		if (error)
			break;
	}

	return !error;
}

/**
 * Sanity check for the global and per-connection HSEP tables.
 * Assertions are made for all these checks. If HSEP is implemented
 * and used correctly, the sanity check will succed.
 *
 * Performed checks (* stands for an arbitrary value):
 *
 * - own triple must be (1, *, *)
 * - global triple for 0 hops must be (0, 0, 0)
 * - per-connection triple for 0 hops must be (0, 0, 0)
 * - per-connection triple for 1 hops must be (1, *, *)
 * - per-connection triples must be monotonically increasing
 * - the sum of the n'th triple of each connection must match the
 *   n'th global table triple for all n
 */

static void
hsep_sanity_check(void)
{
	const GSList *sl;
	hsep_triple sum[G_N_ELEMENTS(hsep_global_table)];
	unsigned int i, j;

	memset(sum, 0, sizeof sum);

	g_assert(1 == hsep_own[HSEP_IDX_NODES]);

	/*
	 * Iterate over all HSEP-capable nodes, and for each triple index
	 * sum up all the connections' triple values.
	 */

	for (sl = node_all_nodes() ; sl; sl = g_slist_next(sl)) {
		struct gnutella_node *n = sl->data;

		/* also consider unestablished connections here */

		if (!(n->attrs & NODE_A_CAN_HSEP))
			continue;

		g_assert(0 == n->hsep->table[0][HSEP_IDX_NODES]);	/* check nodes */
		g_assert(0 == n->hsep->table[0][HSEP_IDX_FILES]);	/* check files */
		g_assert(0 == n->hsep->table[0][HSEP_IDX_KIB]);		/* check KiB */
		g_assert(1 == n->hsep->table[1][HSEP_IDX_NODES]);	/* check nodes */

		/* check if values are monotonously increasing (skip first) */
		g_assert(
			hsep_check_monotony(cast_to_gpointer(n->hsep->table[1]),
				G_N_ELEMENTS(n->hsep->table[1]) - 1)
		);

		/* sum up the values */

		for (i = 0; i < G_N_ELEMENTS(sum); i++) {
			for (j = 0; j < G_N_ELEMENTS(sum[0]); j++)
				sum[i][j] += n->hsep->table[i][j];
		}
	}

	/* check sums */

	for (i = 0; i < G_N_ELEMENTS(sum); i++) {
		for (j = 0; j < G_N_ELEMENTS(sum[0]); j++)
			g_assert(hsep_global_table[i][j] == sum[i][j]);
	}
}

/**
 * Outputs the global HSEP table to the console.
 */

static void
hsep_dump_table(void)
{
	unsigned int i;

	printf("HSEP: Reachable nodes (1-%d hops): ", HSEP_N_MAX);

	for (i = 1; i < G_N_ELEMENTS(hsep_global_table); i++)
		printf("%s ", uint64_to_string(hsep_global_table[i][HSEP_IDX_NODES]));

	printf("\nHSEP: Reachable files (1-%d hops): ", HSEP_N_MAX);

	for (i = 1; i < G_N_ELEMENTS(hsep_global_table); i++)
		printf("%s ", uint64_to_string(hsep_global_table[i][HSEP_IDX_FILES]));

	printf("\nHSEP:   Reachable KiB (1-%d hops): ", HSEP_N_MAX);

	for (i = 1; i < G_N_ELEMENTS(hsep_global_table); i++)
		printf("%s ", uint64_to_string(hsep_global_table[i][HSEP_IDX_KIB]));

	printf("\n");

	hsep_sanity_check();
}

/**
 * Takes a list of triples and returns the optimal number of triples
 * to send in a HSEP message. The number of triples to send
 * is n_opt, defined as (triple indices counted from 0):
 *
 * n_opt := 1 + min {n | triple[n] = triple[k] for all k in [n+1,triples-1]}
 *
 * If there is no such n_opt, n_opt := triples.
 * If all triples are equal, 1 is returned, which is correct.
 *
 * @note
 * This algorithm works regardless of the byte order of the triple data,
 * because only equality tests are used.
 */

static unsigned int
hsep_triples_to_send(const hsep_triple *table, unsigned int triples)
{
	guint i, j, last;
	gboolean changed = FALSE;

	g_assert(table);

	last = triples > 0 ? triples - 1 : 0; /* handle special case */

	/*
	 * We go backwards until we find a triple where at least one of its
	 * components is different from the previously checked triple.
	 */

	for (i = last; i-- > 0; triples--) {

		for (j = 0; j < G_N_ELEMENTS(table[0]); j++)
	   		changed |= table[i][j] != table[last][j];

		if (changed)
			break;
	}

	return triples;
}


/**
 * Initializes HSEP.
 */

void
hsep_init(void)
{
	header_features_add(FEATURES_CONNECTIONS,
		"HSEP", HSEP_VERSION_MAJOR, HSEP_VERSION_MINOR);
	hsep_global_table_changed_event = event_new("hsep_global_table_changed");
 	hsep_fire_global_table_changed(tm_time());
}

/**
 * Adds the specified listener to the list of subscribers for
 * global HSEP table change events. The specified callback is
 * called once immediately, independent of the given frequency type
 * and time interval. This function must be called after hsep_init()
 * has been called.
 */

void
hsep_add_global_table_listener(GCallback cb, frequency_t t, guint32 interval)
{
	hsep_triple table[G_N_ELEMENTS(hsep_global_table)];
	hsep_global_listener_t func = (hsep_global_listener_t) cb;


	/* add callback to the event subscriber list */
	event_add_subscriber(hsep_global_table_changed_event, cb, t, interval);

	/*
	 * Fire up the first event to the specified callback. We do it
	 * manually, because we don't want to fire all listeners, but
	 * just the newly added one, and we want it independent of the
	 * given callback call constraints.
	 */

	hsep_get_global_table(table, G_N_ELEMENTS(table));
	func(table, G_N_ELEMENTS(table));
}

void
hsep_remove_global_table_listener(GCallback cb)
{
	event_remove_subscriber(hsep_global_table_changed_event, cb);
}

/**
 * Resets all HSEP data. The global HSEP table and all connections'
 * HSEP tables are reset to zero. The number of own shared files and
 * kibibytes is untouched. This can be used to watch how quickly
 * the HSEP data converges back to the correct "static" state. As soon
 * as we have received a HSEP message from each of our peers, this state
 * should be reached. Use with care, because this reset will temporarily
 * affect all HSEP-capable nodes in the radius of N_MAX hops!
 */

void
hsep_reset(void)
{
	const GSList *sl;
	guint i;

	memset(hsep_global_table, 0, sizeof hsep_global_table);

	for (sl = node_all_nodes(); sl; sl = g_slist_next(sl)) {
		struct gnutella_node *n = sl->data;

		/* also consider unestablished connections here */

		if (!(n->attrs & NODE_A_CAN_HSEP))
			continue;

		g_assert(n->hsep);

		memset(n->hsep->table, 0, sizeof n->hsep->table);
		memset(n->hsep->sent_table, 0, sizeof n->hsep->sent_table);

		/* this is what we know before receiving the first message */

		for (i = 1; i < G_N_ELEMENTS(hsep_global_table); i++) {
			n->hsep->table[i][HSEP_IDX_NODES] = 1;
			hsep_global_table[i][HSEP_IDX_NODES]++;
		}

		/*
		 * There's no need to reset the last_sent timestamp.
		 * If we'd do this, hsep_timer() would send a message
		 * to all HSEP connections the next time it is called.
		 */
	}
	hsep_fire_global_table_changed(tm_time());
}

/**
 * Initializes the connection's HSEP data.
 */

void
hsep_connection_init(struct gnutella_node *n, guint8 major, guint8 minor)
{
	static const hsep_ctx_t zero_hsep;
	time_t now = tm_time();
	guint i;

	g_assert(n);

	if (GNET_PROPERTY(hsep_debug) > 1)
		printf("HSEP: Initializing node %s\n",
			host_addr_port_to_string(n->addr, n->port));

	n->hsep = walloc(sizeof *n->hsep);
	*n->hsep = zero_hsep; /* Initializes everything to 0 */
	n->hsep->last_sent = now;
	n->hsep->major = major;
	n->hsep->minor = minor;

	/* this is what we know before receiving the first message */

	for (i = 1; i < G_N_ELEMENTS(hsep_global_table); i++) {
		n->hsep->table[i][HSEP_IDX_NODES] = 1;
		hsep_global_table[i][HSEP_IDX_NODES]++;
	}


	hsep_sanity_check();

	hsep_fire_global_table_changed(now);
}

/**
 * Sends a HSEP message to all nodes where the last message
 * has been sent some time ago. This should be called frequently
 * (e.g. every second or every few seconds).
 */

void
hsep_timer(time_t now)
{
	const GSList *sl;
	gboolean scanning_shared;
	static time_t last_sent = 0;

	/* update number of shared files and KiB */

	gnet_prop_get_boolean_val(PROP_LIBRARY_REBUILDING, &scanning_shared);

	if (!scanning_shared) {
		if (upload_is_enabled())
			hsep_notify_shared(shared_files_scanned(), shared_kbytes_scanned());
		else
			hsep_notify_shared(0UL, 0UL);
	}

	for (sl = node_all_nodes(); sl; sl = g_slist_next(sl)) {
		struct gnutella_node *n = sl->data;
		int diff;

		/* only consider established connections here */
		if (!NODE_IS_ESTABLISHED(n))
			continue;

		if (!(n->attrs & NODE_A_CAN_HSEP))
			continue;

		/* check how many seconds ago the last message was sent */
		diff = n->hsep->random_skew + delta_time(now, n->hsep->last_sent);

		/* the -900 is used to react to changes in system time */
		if (diff >= HSEP_MSG_INTERVAL || diff < -900)
			hsep_send_msg(n, now);
	}

	/*
	 * Quick'n dirty hack to update the horizon stats in the
	 * statusbar at least once every 3 seconds.
	 *
	 * TODO: remove this and implement it properly in the
	 * statusbar code.
	 */

	if (delta_time(now, last_sent) >= 3) {
		hsep_fire_global_table_changed(now);
		last_sent = now;
	}
}

/**
 * Updates the global HSEP table when a connection is about
 * to be closed. The connection's HSEP data is restored to
 * zero and the CAN_HSEP attribute is cleared.
 */
void
hsep_connection_close(struct gnutella_node *n, gboolean in_shutdown)
{
	unsigned int i, j;

	g_assert(n);
	g_assert(n->hsep);

	if (GNET_PROPERTY(hsep_debug) > 1)
		printf("HSEP: Deinitializing node %s\n",
			host_addr_port_to_string(n->addr, n->port));

	if (in_shutdown)
		goto cleanup;

	for (i = 1; i < G_N_ELEMENTS(hsep_global_table); i++) {

		for (j = 0; j < G_N_ELEMENTS(hsep_global_table[0]); j++) {
			hsep_global_table[i][j] -= n->hsep->table[i][j];
			n->hsep->table[i][j] = 0;
		}
	}

	if (GNET_PROPERTY(hsep_debug) > 1)
		hsep_dump_table();

	hsep_fire_global_table_changed(tm_time());

	/*
	 * Clear CAN_HSEP attribute so that the HSEP code
	 * will not use the node any longer.
	 */

cleanup:
	n->attrs &= ~NODE_A_CAN_HSEP;
	wfree(n->hsep, sizeof *n->hsep);
	n->hsep = NULL;
}

static inline void
hsep_fix_endian(hsep_triple *messaget, size_t n)
{
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
	(void) messaget;
	(void) n;
#else
	size_t i, j;

	/*
	 * Convert message from little endian to native byte order.
	 * Only the part of the message we are using is converted.
	 * If native byte order is little endian, do nothing.
	 */

	for (i = 0; i < n; i++) {
		for (j = 0; j < G_N_ELEMENTS(messaget[0]); j++) {
			poke_le64(&messaget[i][j], messaget[i][j]);
		}
	}
#endif	/* LITTLE ENDIAN */
}

/**
 * Processes a received HSEP message by updating the
 * connection's and the global HSEP table.
 */

void
hsep_process_msg(struct gnutella_node *n, time_t now)
{
	unsigned int i, j, k, max, msgmax, length;
	hsep_triple *messaget;
	hsep_ctx_t *hsep;

	g_assert(n);
	g_assert(n->hsep);

	hsep = n->hsep;
	length = n->size;

	/* note the offset between message and local data by 1 triple */

	messaget = cast_to_gpointer(n->data);

	if (length == 0) {   /* error, at least 1 triple must be present */
		if (GNET_PROPERTY(hsep_debug) > 1)
			printf("HSEP: Node %s sent empty message\n",
				host_addr_port_to_string(n->addr, n->port));

		return;
	}

	if (length % 24) {   /* error, # of triples not an integer */
		if (GNET_PROPERTY(hsep_debug) > 1)
			printf("HSEP: Node %s sent broken message\n",
				host_addr_port_to_string(n->addr, n->port));

		return;
	}

	/* get N_MAX of peer servent (other_n_max) */
	msgmax = length / 24;

	if (NODE_IS_LEAF(n) && msgmax > 1) {
		if (GNET_PROPERTY(hsep_debug) > 1) {
			printf(
				"HSEP: Node %s is a leaf, but sent %u triples instead of 1\n",
				host_addr_port_to_string(n->addr, n->port), msgmax);
		}
		return;
	}

	/* truncate if peer servent sent more triples than we need */
	max = MIN(msgmax, HSEP_N_MAX);
	hsep_fix_endian(messaget, max);

	/*
	 * Perform sanity check on received message.
	 */

	if (messaget[0][HSEP_IDX_NODES] != 1) { /* # of nodes for 1 hop must be 1 */
		if (GNET_PROPERTY(hsep_debug) > 1)
			printf("HSEP: Node %s's message's #nodes for 1 hop is not 1\n",
				host_addr_port_to_string(n->addr, n->port));
		return;
	}

	if (!hsep_check_monotony(messaget, max)) {
		if (GNET_PROPERTY(hsep_debug) > 1)
			printf("HSEP: Node %s's message's monotony check failed\n",
				host_addr_port_to_string(n->addr, n->port));

		return;
	}

	if (GNET_PROPERTY(hsep_debug) > 1) {
		printf("HSEP: Received %d %s from node %s (msg #%u): ", max,
		    max == 1 ? "triple" : "triples",
			host_addr_port_to_string(n->addr, n->port),
			hsep->msgs_received + 1);
	}

	/*
	 * Update global and per-connection tables.
	 */

	for (k = 0, i = 1; k < max; k++, i++) {

		if (GNET_PROPERTY(hsep_debug) > 1) {
			char buf[G_N_ELEMENTS(messaget[0])][32];

			for (j = 0; j < G_N_ELEMENTS(buf); j++)
				uint64_to_string_buf(messaget[k][j], buf[j], sizeof buf[0]);

			STATIC_ASSERT(3 == G_N_ELEMENTS(buf));
			printf("(%s, %s, %s) ", buf[0], buf[1], buf[2]);
		}

		for (j = 0; j < G_N_ELEMENTS(hsep_global_table[0]); j++) {
			hsep_global_table[i][j] += messaget[k][j] - hsep->table[i][j];
			hsep->table[i][j] = messaget[k][j];
		}
	}

	if (GNET_PROPERTY(hsep_debug) > 1)
		puts("\n");

	/*
	 * If the peer servent sent less triples than we need,
	 * repeat the last triple until we have enough triples
	 */

	/* Go back to last triple */
	if (k > 0)
		k--;

	for (/* NOTHING */; i < G_N_ELEMENTS(hsep_global_table); i++) {

		for (j = 0; j < G_N_ELEMENTS(hsep_global_table[0]); j++) {
			hsep_global_table[i][j] += messaget[k][j] - hsep->table[i][j];
			hsep->table[i][j] = messaget[k][j];
		}
	}

	/*
	 * Update counters and timestamps.
	 */

	hsep->msgs_received++;
	hsep->triples_received += msgmax;

	hsep->last_received = now;

	if (GNET_PROPERTY(hsep_debug) > 1)
		hsep_dump_table();

	hsep_fire_global_table_changed(now);
}

/**
 * Sends a HSEP message to the given node, but only if data to send
 * has changed. Should be called about every 30-60 seconds per node.
 * Will automatically be called by hsep_timer() and
 * hsep_connection_init(). Node must be HSEP-capable.
 */

void
hsep_send_msg(struct gnutella_node *n, time_t now)
{
	hsep_triple tmp[G_N_ELEMENTS(n->hsep->sent_table)], other;
	unsigned int i, j, msglen, triples, opttriples;
	gnutella_msg_hsep_t *msg;
	hsep_ctx_t *hsep;

	g_assert(n);
	g_assert(n->hsep);

	hsep = n->hsep;

	/*
	 * If we are a leaf, we just need to send one triple,
	 * which contains our own data (this triple is expanded
	 * to the needed number of triples on the peer's side).
	 * As the 0'th global and 0'th connection triple are zero,
	 * it contains only our own triple, which is correct.
	 */

	triples = NODE_P_LEAF == GNET_PROPERTY(current_peermode)
				? 1
				: G_N_ELEMENTS(tmp);

	/*
	 * Allocate and initialize message to send.
	 */

	msglen = GTA_HEADER_SIZE + triples * (sizeof *msg - GTA_HEADER_SIZE);
	msg = g_malloc(msglen);

	{
		gnutella_header_t *header;
		
		header = gnutella_msg_hsep_header(msg);
		message_set_muid(header, GTA_MSG_HSEP_DATA);
		gnutella_header_set_function(header, GTA_MSG_HSEP_DATA);
		gnutella_header_set_ttl(header, 1);
		gnutella_header_set_hops(header, 0);
	}

	/*
	 * Collect HSEP data to send and convert the data to
	 * little endian byte order.
	 */

	if (triples > 1) {
		/* determine what we know about non-HSEP nodes in 1 hop distance */
		hsep_get_non_hsep_triple(&other);
	}

	for (i = 0; i < triples; i++) {
		for (j = 0; j < G_N_ELEMENTS(other); j++) {
			guint64 val;

			val = hsep_own[j] + (0 == i ? 0 : other[j]) +
				hsep_global_table[i][j] - hsep->table[i][j];
			poke_le64(&tmp[i][j], val);
		}
	}

	STATIC_ASSERT(sizeof hsep->sent_table == sizeof tmp);
	/* check if the table differs from the previously sent table */
	if (
		0 == memcmp(tmp, hsep->sent_table, sizeof tmp)
	) {
		G_FREE_NULL(msg);
		goto charge_timer;
	}

	memcpy(cast_to_gchar_ptr(msg) + GTA_HEADER_SIZE,
		tmp, triples * sizeof tmp[0]);

	/* store the table for later comparison */
	memcpy(hsep->sent_table, tmp, triples * sizeof tmp[0]);

	/*
	 * Note that on big endian architectures the message data is now in
	 * the wrong byte order. Nevertheless, we can use hsep_triples_to_send()
	 * with that data.
	 */

	/* optimize number of triples to send */
	opttriples = hsep_triples_to_send(cast_to_gpointer(tmp), triples);

	if (GNET_PROPERTY(hsep_debug) > 1) {
		printf("HSEP: Sending %d %s to node %s (msg #%u): ", opttriples,
		    opttriples == 1 ? "triple" : "triples",
			host_addr_port_to_string(n->addr, n->port),
			hsep->msgs_sent + 1);
	}

	for (i = 0; i < opttriples; i++) {
		if (GNET_PROPERTY(hsep_debug) > 1) {
			char buf[G_N_ELEMENTS(hsep_own)][32];

			for (j = 0; j < G_N_ELEMENTS(buf); j++) {
				guint64 v;

				v = hsep_own[j] + hsep_global_table[i][j] - hsep->table[i][j];
				uint64_to_string_buf(v, buf[j], sizeof buf[0]);
			}

			STATIC_ASSERT(3 == G_N_ELEMENTS(buf));
			printf("(%s, %s, %s) ", buf[0], buf[1], buf[2]);
		}
	}

	if (GNET_PROPERTY(hsep_debug) > 1)
		puts("\n");

	/* write message size */
	msglen = opttriples * 24;
	gnutella_header_set_size(gnutella_msg_hsep_header(msg), msglen);

	/* correct message length */
	msglen += GTA_HEADER_SIZE;

	/* send message to peer node */
	gmsg_sendto_one(n, msg, msglen);

	G_FREE_NULL(msg);

	/*
	 * Update counters.
	 */

	hsep->msgs_sent++;
	hsep->triples_sent += opttriples;

charge_timer:

	hsep->last_sent = now;
	hsep->random_skew = random_value(2 * HSEP_MSG_SKEW) - HSEP_MSG_SKEW;
}

/**
 * This should be called whenever the number of shared files or kibibytes
 * change. The values are checked for changes, nothing is done if nothing
 * has changed. Note that kibibytes are determined by shifting the number
 * of bytes right by 10 bits, not by dividing by 1000.
 */

void
hsep_notify_shared(guint64 own_files, guint64 own_kibibytes)
{
	/* check for change */
	if (
		own_files != hsep_own[HSEP_IDX_FILES] ||
		own_kibibytes != hsep_own[HSEP_IDX_KIB]
	) {

		if (GNET_PROPERTY(hsep_debug)) {
			printf("HSEP: Shared files changed to %s (%s KiB)\n",
			    uint64_to_string(own_files), uint64_to_string2(own_kibibytes));
		}

		hsep_own[HSEP_IDX_FILES] = own_files;
		hsep_own[HSEP_IDX_KIB] = own_kibibytes;

		/*
		 * We could send a HSEP message to all nodes now, but these changes
		 * will propagate within at most HSEP_MSG_INTERVAL + HSEP_MSG_SKEW
		 * seconds anyway.
		 */
	}
}

/**
 * Copies the first maxtriples triples from the global HSEP table into
 * the specified buffer. If maxtriples is larger than the number of
 * triples in the table, it is truncated appropriately. Note that also
 * the 0'th triple is copied, which is always zero.
 *
 * @return The number of copied triples.
 */

unsigned int
hsep_get_global_table(hsep_triple *buffer, unsigned int maxtriples)
{
	g_assert(buffer);

	maxtriples = MIN(maxtriples, G_N_ELEMENTS(hsep_global_table));
	memcpy(buffer, hsep_global_table, maxtriples * sizeof buffer[0]);

	return maxtriples;
}

/**
 * Copies the first maxtriples triples from the connection's HSEP table into
 * the specified buffer. If maxtriples is larger than the number of
 * triples in the table, it is truncated appropriately. Note that also
 * the 0'th triple is copied, which is always zero.
 *
 * @return The number of copied triples.
 */

unsigned int
hsep_get_connection_table(const struct gnutella_node *n,
    hsep_triple *buffer, unsigned int maxtriples)
{
	g_assert(n);
	g_assert(n->hsep);
	g_assert(buffer);

	maxtriples = MIN(maxtriples, G_N_ELEMENTS(n->hsep->table));
	memcpy(buffer, n->hsep->table, maxtriples * sizeof buffer[0]);

	return maxtriples;
}

/**
 * Used to shutdown HSEP.
 */

void
hsep_close(void)
{
	event_destroy(hsep_global_table_changed_event);
}

#if 0 /* UNUSED */
/**
 * Checks whether the global HSEP table has changed since the
 * specified point in time. Returns TRUE if this is the case,
 * FALSE otherwise.
 */

gboolean
hsep_has_global_table_changed(time_t since)
{
	return delta_time(hsep_last_global_table_change, since) > 0;
}
#endif /* UNUSED */

/**
 * Gets a HSEP-compatible triple for all non-HSEP nodes.
 * The number of nodes is just the number of established non-HSEP
 * connections, the number of shared files and KiB is the
 * sum of the known PONG-based library sizes of those connections.
 * Note that this takes only direct neighbor connections into
 * account. Also note that the shared library size in KiB is
 * not accurate due to Gnutella protocol limitations.
 *
 * The determined values are stored in the provided triple address.
 */

void
hsep_get_non_hsep_triple(hsep_triple *tripledest)
{
	const GSList *sl;
	guint64 other_nodes = 0;      /* # of non-HSEP nodes */
	guint64 other_files = 0;      /* what non-HSEP nodes share (files) */
	guint64 other_kib = 0;        /* what non-HSEP nodes share (KiB) */

	g_assert(tripledest);

	/*
	 * Iterate over all established non-HSEP nodes and count these nodes and
	 * sum up what they share (PONG-based library size).
	 */

	for (sl = node_all_nodes() ; sl; sl = g_slist_next(sl)) {
		struct gnutella_node *n = sl->data;
		gnet_node_status_t status;

		if ((!NODE_IS_ESTABLISHED(n)) || n->attrs & NODE_A_CAN_HSEP)
			continue;

		other_nodes++;

		if (!node_get_status(NODE_ID(n), &status))
			continue;

		if (status.gnet_info_known) {
			other_files += status.gnet_files_count;
			other_kib += status.gnet_kbytes_count;
		}
	}

	tripledest[0][HSEP_IDX_NODES] = other_nodes;
	tripledest[0][HSEP_IDX_FILES] = other_files;
	tripledest[0][HSEP_IDX_KIB] = other_kib;
}


/**
 * @returns a static string of the cell contents of the given row and column.
 *
 * @attention
 * NB: The static buffers for each column are disjunct.
 */
const char *
hsep_get_static_str(int row, int column)
{
	const char *ret = NULL;
	hsep_triple hsep_table[G_N_ELEMENTS(hsep_global_table)];
	hsep_triple other[1];
	guint64 v;

	hsep_get_global_table(hsep_table, G_N_ELEMENTS(hsep_table));
	hsep_get_non_hsep_triple(other);

    switch (column) {
    case HSEP_IDX_NODES:
		{
			static char buf[UINT64_DEC_BUFLEN];

			v = hsep_table[row][HSEP_IDX_NODES] + other[0][HSEP_IDX_NODES];
			uint64_to_string_buf(v, buf, sizeof buf);
			ret = buf;
		}
		break;

    case HSEP_IDX_FILES:
		{
			static char buf[UINT64_DEC_BUFLEN];

			v = hsep_table[row][HSEP_IDX_FILES] + other[0][HSEP_IDX_FILES];
			uint64_to_string_buf(v, buf, sizeof buf);
			ret = buf;
		}
		break;

	case HSEP_IDX_KIB:
		{
			static char buf[UINT64_DEC_BUFLEN];

			/* Make a copy because concurrent usage of short_kb_size()
	 	 	 * could be hard to discover. */
			v = hsep_table[row][HSEP_IDX_KIB] + other[0][HSEP_IDX_KIB];
			g_strlcpy(buf,
				short_kb_size(v, GNET_PROPERTY(display_metric_units)),
				sizeof buf);
  			ret = buf;
		}
		break;
    }

	g_assert(ret != NULL);
	return ret;
}

/**
 * @returns the size of the global hsep table
 */
int
hsep_get_table_size(void)
{
	hsep_triple hsep_table[G_N_ELEMENTS(hsep_global_table)];

	hsep_get_global_table(hsep_table, G_N_ELEMENTS(hsep_table));
	return G_N_ELEMENTS(hsep_table);
}

/* vi: set ts=4 sw=4 cindent: */
