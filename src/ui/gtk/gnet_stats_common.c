/*
 * $Id$
 *
 * Copyright (c) 2001-2003, Richard Eckart
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

#include "gui.h"

RCSID("$Id$")

#include "gtk/notebooks.h"

#include "gnet_stats_common.h"
#include "settings.h"

#include "if/core/net_stats.h"
#include "if/bridge/ui2c.h"

#include "lib/glib-missing.h"
#include "lib/stringify.h"
#include "lib/override.h"		/* Must be the last header included */

/**
 * Gets the string associated with the message type.
 */
const gchar *
msg_type_str(gint value)
{
	static const char * const strs[] = {
		N_("Unknown"),
		N_("Ping"),
		N_("Pong"),
		N_("Bye"),
		N_("QRP"),
		N_("HSEP"),
		N_("RUDP"),
		N_("Vendor spec."),
		N_("Vendor std."),
		N_("Push"),
		N_("Query"),
		N_("Query hit"),
		N_("DHT"),
		N_("Total"),
	};

	STATIC_ASSERT(G_N_ELEMENTS(strs) == MSG_TYPE_COUNT);

	if ((guint) value >= G_N_ELEMENTS(strs)) {
		g_warning("Requested general_type_str %d is invalid", value);
		return "";
	}

	return _(strs[value]);
}

gint
msg_type_str_size(void)
{
	return MSG_TYPE_COUNT;
}

/**
 * Gets the string associated with the drop reason.
 */
const gchar *
msg_drop_str(gint value)
{
	g_return_val_if_fail(UNSIGNED(value) < MSG_DROP_REASON_COUNT, "");
	return _(guc_gnet_stats_drop_reason_to_string(value));
}

/**
 * Gets the string associated with the general message.
 */
const gchar *
general_type_str(gint value)
{
	static const char * const strs[] = {
		N_("Routing errors"),
		N_("Searches to local DB"),
		N_("Hits on local DB"),
		N_("Query hits received for local queries"),
		N_("Query hits received for OOB-proxied queries"),
		N_("Queries requesting OOB hit delivery"),
		N_("Stripped OOB flag on queries"),
		N_("Duplicates with higher TTL"),
		N_("Duplicate OOB-proxied queries"),
		N_("OOB hits received for OOB-proxied queries"),
		N_("OOB hits bearing alien IP address"),
		N_("Unclaimed locally-generated OOB hits"),
		N_("Partially claimed locally-generated OOB hits"),
		N_("Spurious OOB hit claiming received"),
		N_("Unrequested OOB hits received"),
		N_("Compacted queries"),
		N_("Bytes saved by compacting"),
		N_("UTF8 queries"),
		N_("SHA1 queries"),
		N_("Broadcasted push messages"),
		N_("Push-proxy UDP relayed messages"),
		N_("Push-proxy TCP relayed messages"),
		N_("Push-proxy broadcasted messages"),
		N_("Push-proxy found un-proxied local route"),
		N_("Push-proxy lookup failures"),
		N_("Push relayed via local route"),
		N_("Push relayed via routing table"),
		N_("Locally generated dynamic queries"),
		N_("Leaf-generated dynamic queries"),
		N_("OOB-proxied leaf queries"),
		N_("Fully completed dynamic queries"),
		N_("Partially completed dynamic queries"),
		N_("Dynamic queries ended with no results"),
		N_("Fully completed dynamic queries getting late results"),
		N_("Dynamic queries with partial late results"),
		N_("Dynamic queries completed by late results"),
		N_("Queries seen from GTKG"),
		N_("Queries seen from GTKG that were re-queries"),
		N_("Queries advertising support of GGEP \"H\""),
		N_("GIV callbacks received"),
		N_("GIV discarded due to no suitable download"),
		N_("QUEUE callbacks received"),
		N_("QUEUE discarded due to no suitable download"),
		N_("UDP messages with bogus source IP"),
		N_("Alien UDP messages (non-Gnutella)"),
		N_("Unprocessed UDP Gnutella messages"),
		N_("Compressed UDP messages enqueued"),
		N_("Compressed UDP messages received"),
		N_("Uncompressed UDP messages due to no gain"),
		N_("Consolidated servers (after GUID and IP address linking)"),
		N_("Duplicate downloads found during server consolidation"),
		N_("Discovered server GUIDs"),
		N_("Changed server GUIDs"),
		N_("Detected GUID collisions"),
		N_("Detected collisions with our own GUID"),
		N_("Firewalled node info for known hosts received in upload requests"),
		N_("Revitalized PUSH routes"),
		N_("Attempted download resource switching on completion"),
		N_("Attempted download resource switching after error"),
		N_("Successful download resource switching (all kind)"),
		N_("Successful download resource switching between plain files"),
		N_("Successful download resource switching after error"),
		N_("Actively queued after resource switching attempt"),
		N_("Sunk HTTP reply data on error codes"),
		N_("Ignored downloaded data"),
		N_("Ignoring requested after data mismatch"),
		N_("Ignoring requested to preserve connection"),
		N_("Ignoring requested due to aggressive swarming"),
		N_("Ignoring refused (data too large or server too slow)"),
		N_("Client resource switching (all detected)"),
		N_("Client resource switching between plain files"),
		N_("Client follow-up request after HTTP error was returned"),
		N_("PARQ client resource switching in slots (SHA-1 based)"),
		N_("PARQ client retry-after violation"),
		N_("PARQ client kicked out after too many retry-after violations"),
		N_("PARQ upload slot limit overrides"),
		N_("PARQ quick upload slots granted"),
		N_("PARQ QUEUE sending attempts"),
		N_("PARQ QUEUE messages sent"),
		N_("PARQ QUEUE follow-up requests received"),
		N_("Launched SHA-1 file verifications"),
		N_("Launched TTH file verifications"),
		N_("Re-seeding of orphan downloads through query hits"),
		N_("Re-seeding of orphan downloads through upload requests"),
		N_("DHT estimated amount of nodes"),
		N_("DHT k-ball furthest frontier (bits)"),
		N_("DHT k-ball closeest frontier (bits)"),
		N_("DHT routing table buckets"),
		N_("DHT routing table leaves"),
		N_("DHT routing table maximum depth"),
		N_("DHT routing table good nodes"),
		N_("DHT routing table stale nodes"),
		N_("DHT routing table pending nodes"),
		N_("DHT routing table evicted nodes"),
		N_("DHT routing table evicted firewalled nodes"),
		N_("DHT routing table promoted pending nodes"),
		N_("DHT routing table pinged promoted nodes"),
		N_("DHT completed bucket refreshes"),
		N_("DHT forced bucket refreshes"),
		N_("DHT denied non-splitable bucket refresh"),
		N_("DHT initiated bucket alive checks"),
		N_("DHT alive pings sent to good nodes"),
		N_("DHT alive pings sent to stale nodes"),
		N_("DHT value store rejected on IP/network quota grounds"),
		N_("DHT value store rejected on creator validation grounds"),
		N_("DHT keys held"),
		N_("DHT cached keys held"),
		N_("DHT values held"),
		N_("DHT cached KUID targets held"),
		N_("DHT cached closest root nodes"),
		N_("DHT cached roots exact hits"),
		N_("DHT cached roots approximate hits"),
		N_("DHT cached roots misses"),
		N_("DHT cached roots lookups within k-ball"),
		N_("DHT cached roots contact address refreshed"),
		N_("DHT cached security tokens held"),
		N_("DHT cached security tokens hits"),
		N_("DHT stable node information held"),
		N_("DHT local hits on value lookups"),
		N_("DHT local hits returning values from cached keys"),
		N_("DHT returned expanded values"),
		N_("DHT returned values as secondary keys"),
		N_("DHT claimed values via secondary keys"),
		N_("DHT returned cached expanded values"),
		N_("DHT returned cached values as secondary-keys"),
		N_("DHT claimed cached values via secondary keys"),
		N_("DHT successful received value publications"),
		N_("DHT successful received value removals"),
		N_("DHT replication of stale value avoided"),
		N_("DHT replication of held values"),
		N_("DHT republishing of held values"),
		N_("DHT secondary-key value fetch issued"),
		N_("DHT duplicate values returned in lookups"),
		N_("DHT detected KUID collisions"),
		N_("DHT detected collisions with our own KUID"),
		N_("DHT detected KUID mismatches on RPC reply"),
		N_("DHT caching attempts"),
		N_("DHT caching ended successfully"),
		N_("DHT caching partially completed"),
		N_("DHT key-offloading checks after discovering new closest node"),
		N_("DHT keys selected for offloading"),
		N_("DHT key-offloading attempts"),
		N_("DHT key-offloading ended successfully"),
		N_("DHT key-offloading partially completed"),
		N_("DHT values successfully offloaded"),
		N_("DHT publishing attempts"),
		N_("DHT publishing ended successfully (all roots)"),
		N_("DHT publishing partially completed (root subset only)"),
		N_("DHT publishing ending with proper value presence"),
		N_("DHT value republishing occurring too late (after expiry)"),
		N_("DHT publishing to self"),
		N_("DHT background publishing completion attempts"),
		N_("DHT background publishing completion showing improvements"),
		N_("DHT background publishing completion successful (all roots)"),
		N_("DHT alt-loc lookups issued"),
		N_("DHT push-proxy lookups issued"),
		N_("DHT successful alt-loc lookups"),
		N_("DHT successful push-proxy lookups"),
		N_("DHT re-seeding of orphan downloads"),
	};

	STATIC_ASSERT(G_N_ELEMENTS(strs) == GNR_TYPE_COUNT);

	if ((guint) value >= G_N_ELEMENTS(strs)) {
		g_warning("Requested general_type_str %d is invalid", value);
		return "";
	}

	return _(strs[value]);
}

/**
 * @returns the cell contents for the horizon stats table.
 *
 * @warning
 * NB: The static buffers for each column are disjunct.
 */
const gchar *
horizon_stat_str(gint row, c_horizon_t column)
{
    switch (column) {
    case c_horizon_hops:
		{
    		static gchar buf[UINT64_DEC_BUFLEN];

			gm_snprintf(buf, sizeof(buf), "%d", row);
           	return buf;
		}
    case c_horizon_nodes:
		{
           	return guc_hsep_get_static_str(row, HSEP_IDX_NODES);
		}
    case c_horizon_files:
		{
           	return guc_hsep_get_static_str(row, HSEP_IDX_FILES);
		}
    case c_horizon_size:
		{
           	return guc_hsep_get_static_str(row, HSEP_IDX_KIB);
		}
    case num_c_horizon:
		g_assert_not_reached();
    }

    return NULL;
}

/**
 * Updates the horizon statistics in the statusbar.
 *
 * This is an event-driven callback called from the HSEP code
 * using the event listener framework. In addition to taking into account
 * the HSEP information, the number of established non-HSEP nodes and
 * their library size (if provided) are added to the values displayed.
 */
void
gnet_stats_gui_horizon_update(hsep_triple *table, guint32 triples)
{
	const guint32 hops = 4U;      /* must be <= HSEP_N_MAX */
	guint64 val;
	hsep_triple other;

	if (triples <= hops)     /* should not happen */
	    return;
	g_assert((gint32) triples > 0);

	guc_hsep_get_non_hsep_triple(&other);

	/*
	 * Update the 3 labels in the statusbar with the horizon values for a
	 * distance of 'hops' hops.
	 */

	val = table[hops][HSEP_IDX_NODES] + other[HSEP_IDX_NODES];
	gtk_label_printf(GTK_LABEL(
			gui_main_window_lookup("label_statusbar_horizon_node_count")),
		"%s %s", uint64_to_string(val), NG_("node", "nodes", val));

	val = table[hops][HSEP_IDX_FILES] + other[HSEP_IDX_FILES];
	gtk_label_printf(GTK_LABEL(
			gui_main_window_lookup("label_statusbar_horizon_file_count")),
		"%s %s", uint64_to_string(val), NG_("file", "files", val));

	val = table[hops][HSEP_IDX_KIB] + other[HSEP_IDX_KIB];
	gtk_label_printf(GTK_LABEL(
			gui_main_window_lookup("label_statusbar_horizon_kb_count")),
		"%s", short_kb_size(val, show_metric_units()));
}

static gboolean
gnet_stats_gui_is_visible(void)
{
	return main_gui_window_visible() &&
		nb_main_page_stats == main_gui_notebook_get_page();
}

void
gnet_stats_gui_timer(time_t now)
{
	static time_t last_update;

	if (last_update != now && gnet_stats_gui_is_visible()) {
		last_update = now;
		gnet_stats_gui_update_display(now);
	}
}

/* vi: set ts=4 sw=4 cindent: */
