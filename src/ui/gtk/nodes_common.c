/*
 * $Id$
 *
 * Copyright (c) 2001-2003, Raphael Manfredi, Richard Eckart
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
 * @ingroup gtk
 * @file
 *
 * Common GUI functions for displaying node information.
 *
 * @author Raphael Manfredi
 * @author Richard Eckart
 * @date 2001-2003
 */

#include "gui.h"

RCSID("$Id$");

#include "nodes_common.h"
#include "settings.h"

#include "gtk/statusbar.h"

#include "if/bridge/ui2c.h"
#include "if/core/nodes.h"
#include "if/core/sockets.h"
#include "if/gui_property_priv.h"

#include "lib/glib-missing.h"
#include "lib/misc.h"
#include "lib/walloc.h"
#include "lib/override.h"	/* Must be the last header included */

static gchar gui_tmp[4096];

/**
 * Compute info string for node.
 *
 * @return pointer to static data.
 */
const gchar *
nodes_gui_common_status_str(const gnet_node_status_t *n)
{
	const gchar *a;

	switch (n->status) {
	case GTA_NODE_CONNECTING:
		a = _("Connecting...");
		break;

	case GTA_NODE_HELLO_SENT:
		a = _("Hello sent");
		break;

	case GTA_NODE_WELCOME_SENT:
		a = _("Welcome sent");
		break;

	case GTA_NODE_CONNECTED:
		if (n->sent || n->received) {
			size_t slen = 0;

			if (!node_show_detailed_info) {
				gm_snprintf(gui_tmp, sizeof(gui_tmp),
					"TX=%d RX=%d Q=%d,%d%% %s",
					n->sent, n->received,
					n->mqueue_count, n->mqueue_percent_used,
					n->in_tx_swift_control ? " [SW]" :
					n->in_tx_flow_control ? " [FC]" : "");
				a = gui_tmp;
				break;
			}

			if (n->tx_compressed && show_gnet_info_txc)
				slen += gm_snprintf(gui_tmp, sizeof(gui_tmp), "TXc=%d,%d%%",
					(gint) n->sent, (gint) (n->tx_compression_ratio * 100));
			else
				slen += gm_snprintf(gui_tmp, sizeof(gui_tmp), "TX=%d",
							(gint) n->sent);

			if (show_gnet_info_tx_speed || show_gnet_info_tx_wire) {
				gboolean is_first = TRUE;

				slen += gm_snprintf(&gui_tmp[slen], sizeof(gui_tmp)-slen,
					" (" /* ')' */);

				if (show_gnet_info_tx_wire) {
					slen += gm_snprintf(&gui_tmp[slen], sizeof(gui_tmp)-slen,
					"%s", compact_size(n->tx_written, show_metric_units()));
					is_first = FALSE;
				}

				if (show_gnet_info_tx_speed)
					slen += gm_snprintf(&gui_tmp[slen], sizeof(gui_tmp)-slen,
						"%s%s", is_first ? "" : ", ",
						compact_rate(n->tx_bps, show_metric_units()));

				slen += gm_snprintf(&gui_tmp[slen], sizeof(gui_tmp)-slen,
					/* '(' */ ")");
			}

			if (n->rx_compressed && show_gnet_info_rxc)
				slen += gm_snprintf(&gui_tmp[slen], sizeof(gui_tmp)-slen,
					" RXc=%d,%d%%",
					(gint) n->received, (gint) (n->rx_compression_ratio * 100));
			else
				slen += gm_snprintf(&gui_tmp[slen], sizeof(gui_tmp)-slen,
					" RX=%d", (gint) n->received);

			if (show_gnet_info_rx_speed || show_gnet_info_rx_wire) {
				gboolean is_first = TRUE;

				slen += gm_snprintf(&gui_tmp[slen], sizeof(gui_tmp)-slen,
					" (" /* ')' */);

				if (show_gnet_info_rx_wire) {
					slen += gm_snprintf(&gui_tmp[slen], sizeof(gui_tmp)-slen,
						"%s", compact_size(n->rx_given, show_metric_units()));
					is_first = FALSE;
				}

				if (show_gnet_info_rx_speed)
					slen += gm_snprintf(&gui_tmp[slen], sizeof(gui_tmp)-slen,
						"%s%s", is_first ? "" : ", ",
						compact_rate(n->rx_bps, show_metric_units()));

				slen += gm_snprintf(&gui_tmp[slen], sizeof(gui_tmp)-slen,
					/* '(' */ ")");
			}

			if (
				show_gnet_info_tx_queries || show_gnet_info_rx_queries ||
				show_gnet_info_gen_queries || show_gnet_info_sq_queries
			) {
				gboolean is_first = TRUE;

				slen += gm_snprintf(&gui_tmp[slen], sizeof(gui_tmp)-slen,
					" Query(" /* ')' */);

				if (show_gnet_info_gen_queries) {
					slen += gm_snprintf(&gui_tmp[slen], sizeof(gui_tmp)-slen,
						"Gen=%d", n->squeue_sent);
					is_first = FALSE;
				}
				if (show_gnet_info_sq_queries) {
					slen += gm_snprintf(&gui_tmp[slen], sizeof(gui_tmp)-slen,
						"%sQ=%d", is_first ? "" : ", ", n->squeue_count);
					is_first = FALSE;
				}
				if (show_gnet_info_tx_queries) {
					slen += gm_snprintf(&gui_tmp[slen], sizeof(gui_tmp)-slen,
						"%sTX=%u", is_first ? "" : ", ", n->tx_queries);
					is_first = FALSE;
				}
				if (show_gnet_info_rx_queries)
					slen += gm_snprintf(&gui_tmp[slen], sizeof(gui_tmp)-slen,
						"%sRX=%u", is_first ? "" : ", ", n->rx_queries);

				slen += gm_snprintf(&gui_tmp[slen], sizeof(gui_tmp)-slen,
					/* '(' */ ")");
			}

			if (show_gnet_info_tx_hits || show_gnet_info_rx_hits) {
				gboolean is_first = TRUE;

				slen += gm_snprintf(&gui_tmp[slen], sizeof(gui_tmp)-slen,
					" QHit(" /* ')' */);

				if (show_gnet_info_tx_hits) {
					slen += gm_snprintf(&gui_tmp[slen], sizeof(gui_tmp)-slen,
						"TX=%u", n->tx_qhits);
					is_first = FALSE;
				}
				if (show_gnet_info_rx_hits)
					slen += gm_snprintf(&gui_tmp[slen], sizeof(gui_tmp)-slen,
						"%sRX=%u", is_first ? "" : ", ", n->rx_qhits);

				slen += gm_snprintf(&gui_tmp[slen], sizeof(gui_tmp)-slen,
					/* '(' */ ")");
			}

			if (show_gnet_info_tx_dropped || show_gnet_info_rx_dropped) {
				gboolean is_first = TRUE;

				slen += gm_snprintf(&gui_tmp[slen], sizeof(gui_tmp)-slen,
					" Drop(" /* ')' */);

				if (show_gnet_info_tx_dropped) {
					slen += gm_snprintf(&gui_tmp[slen], sizeof(gui_tmp)-slen,
						"TX=%u", n->tx_dropped);
					is_first = FALSE;
				}
				if (show_gnet_info_rx_dropped)
					slen += gm_snprintf(&gui_tmp[slen], sizeof(gui_tmp)-slen,
						"%sRX=%u", is_first ? "" : ", ", n->rx_dropped);

				slen += gm_snprintf(&gui_tmp[slen], sizeof(gui_tmp)-slen,
					/* '(' */ ")");
			}

			if (show_gnet_info_shared_size || show_gnet_info_shared_files) {
				gboolean is_first = TRUE;

				slen += gm_snprintf(&gui_tmp[slen], sizeof(gui_tmp)-slen,
					" Lib(" /* ')' */);

				if (show_gnet_info_shared_size && n->gnet_info_known) {
					slen += gm_snprintf(&gui_tmp[slen], sizeof(gui_tmp)-slen,
						"%s",
						compact_kb_size(n->gnet_files_count
							? n->gnet_kbytes_count : 0, show_metric_units()));
					is_first = FALSE;
				}
				if (show_gnet_info_shared_files && n->gnet_info_known)
					slen += gm_snprintf(&gui_tmp[slen], sizeof(gui_tmp)-slen,
						"%s#=%u", is_first ? "" : ", ", n->gnet_files_count);

				slen += gm_snprintf(&gui_tmp[slen], sizeof(gui_tmp)-slen,
					/* '(' */ "%s)", n->gnet_info_known ? "" : "?");
			}

			if (show_gnet_info_qrp_stats) {
				if (n->has_qrp)
					slen += gm_snprintf(&gui_tmp[slen], sizeof(gui_tmp)-slen,
						" QRP=%u%%",
						(guint) (n->qrp_efficiency * 100.0));

				if (n->qrt_slots != 0)
					slen += gm_snprintf(&gui_tmp[slen], sizeof(gui_tmp)-slen,
						" QRT(%s, g=%d, f=%d%%, t=%d%%, e=%d%%)",
						compact_size(n->qrt_slots, show_metric_units()),
						n->qrt_generation,
						n->qrt_fill_ratio, n->qrt_pass_throw,
						(guint) (n->qrp_efficiency * 100.0));
			}

			if (show_gnet_info_dbw)
				slen += gm_snprintf(&gui_tmp[slen], sizeof(gui_tmp)-slen,
				" Dup=%u Bad=%u W=%u H=%u S=%u",
				n->n_dups, n->n_bad, n->n_weird, n->n_hostile, n->n_spam);

			if (show_gnet_info_rt) {
				slen += gm_snprintf(&gui_tmp[slen], sizeof(gui_tmp)-slen,
				" RT(avg=%d, last=%d", n->rt_avg, n->rt_last);	/* ) */
				if (n->tcp_rtt)
					slen += gm_snprintf(&gui_tmp[slen], sizeof(gui_tmp)-slen,
						", tcp=%d", n->tcp_rtt);
				if (n->udp_rtt)
					slen += gm_snprintf(&gui_tmp[slen], sizeof(gui_tmp)-slen,
						", udp=%d", n->udp_rtt);
				slen += gm_snprintf(&gui_tmp[slen], sizeof(gui_tmp)-slen,
					/* ( */ ")");
			}

			slen += gm_snprintf(&gui_tmp[slen], sizeof(gui_tmp)-slen,
				" Q=%d,%d%% %s",
				n->mqueue_count, n->mqueue_percent_used,
				n->in_tx_swift_control ? " [SW]" :
				n->in_tx_flow_control ? " [FC]" : "");
			a = gui_tmp;
		} else
			a = _("Connected");
		break;

	case GTA_NODE_SHUTDOWN:
		{
			gm_snprintf(gui_tmp, sizeof(gui_tmp),
				_("Closing: %s [Stop in %ds] RX=%d Q=%d,%d%%"),
				n->message, n->shutdown_remain, n->received,
				n->mqueue_count, n->mqueue_percent_used);
			a = gui_tmp;
		}
		break;

	case GTA_NODE_REMOVING:
		a =  *n->message ? n->message : _("Removing");
		break;

	case GTA_NODE_RECEIVING_HELLO:
		a = _("Receiving hello");
		break;

	default:
		a = _("UNKNOWN STATUS");
	}

	return a;
}

/**
 * Display a summary of the node flags.
 *
 * The stuff in the Flags column means:
 *
 *  - 012345678AB (offset)
 *  - NIrwqxZPFhE
 *  - ^^^^^^^^^^^
 *  - ||||||||||+ E indicates a TLS encrypted connection
 *  - |||||||||+  hops flow triggerd (h), or total query flow control (f)
 *  - ||||||||+   flow control (F), or pending data in queue (d)
 *  - |||||||+    indicates whether we're a push proxy (P) / node is proxy (p)
 *  - ||||||+     indicates whether RX, TX or both (Z) are compressed
 *  - |||||+      indicates whether we sent our last-hop QRT to remote UP
 *  - ||||+       indicates whether we sent/received a QRT, or send/receive one
 *  - |||+        indicates whether node is writable
 *  - ||+         indicates whether node is readable
 *  - |+          indicates connection type (Incoming, Outgoing, Ponging)
 *  - +           indicates peer mode (Normal, Ultra, Leaf)
 */
const gchar *
nodes_gui_common_flags_str(const gnet_node_flags_t *flags)
{
	static gchar status[] = "NIrwqTRPFhS";

	switch (flags->peermode) {
		case NODE_P_UNKNOWN:	status[0] = '-'; break;
		case NODE_P_ULTRA:		status[0] = 'U'; break;
		case NODE_P_NORMAL:		status[0] = 'N'; break;
		case NODE_P_LEAF:		status[0] = 'L'; break;
		case NODE_P_CRAWLER:	status[0] = 'C'; break;
		case NODE_P_UDP:		status[0] = 'P'; break;
		default:				g_assert(0); break;
	}

	status[1] = flags->incoming ? 'I' : 'O';
	status[2] = flags->readable ? 'r' : '-';
	status[3] = flags->writable ? 'w' : '-';

	switch (flags->qrt_state) {
		case QRT_S_SENT: case QRT_S_RECEIVED:		status[4] = 'Q'; break;
		case QRT_S_SENDING: case QRT_S_RECEIVING:	status[4] = 'q'; break;
		case QRT_S_PATCHING:						status[4] = 'p'; break;
		default:									status[4] = '-';
	}

	switch (flags->uqrt_state) {
		case QRT_S_SENT:		status[5] = 'X'; break;
		case QRT_S_SENDING:		status[5] = 'x'; break;
		case QRT_S_PATCHING:	status[5] = 'p'; break;
		default:				status[5] = '-';
	}

	status[6] =
		flags->tx_compressed && flags->rx_compressed ? 'Z' :
		flags->tx_compressed ? 'T' :
		flags->rx_compressed ? 'R' : '-';

	if (flags->is_push_proxied)  status[7] = 'P';
	else if (flags->is_proxying) status[7] = 'p';
	else status[7] = '-';

	if (flags->in_tx_swift_control) status[8]     = 'S';
	else if (flags->in_tx_flow_control) status[8] = 'F';
	else if (flags->mqueue_above_lowat) status[8] = 'D';
	else if (!flags->mqueue_empty) status[8]      = 'd';
	else status[8]                                = '-';

	if (flags->hops_flow == 0)
		status[9] = 'f';
	else if (flags->hops_flow < GTA_NORMAL_TTL)
		status[9] = 'h';
	else
		status[9] = '-';

	status[10] = flags->tls ? 'E' : '-';

	status[sizeof(status) - 1] = '\0';
	return status;
}

struct add_node_context {
	guint32 flags;
	guint16 port;
};

static void
add_node_helper(const host_addr_t *addrs, size_t n, gpointer data)
{
	struct add_node_context *ctx = data;

	g_assert(addrs);
	g_assert(ctx);
	g_assert(0 != ctx->port);

	if (n > 0) {
		guc_node_add(addrs[random_raw() % n], ctx->port, ctx->flags);
	}

	wfree(ctx, sizeof *ctx);
}

/**
 * Try to connect to the node given by the addr string in the form
 * [ip]:[port]. Port may be omitted.
 */
void
nodes_gui_common_connect_by_name(const gchar *line)
{
	const gchar *q;

    g_assert(line);

	q = line;
	while ('\0' != *q) {
		const gchar *endptr, *hostname;
		size_t hostname_len;
		host_addr_t addr;
		guint32 flags;
    	guint16 port;

		q = skip_ascii_spaces(q);
		if (',' == *q) {
			q++;
			continue;
		}

		addr = zero_host_addr;
		port = GTA_PORT;
		flags = CONNECT_F_FORCE;
		endptr = NULL;
		hostname = NULL;
		hostname_len = 0;

		endptr = is_strcaseprefix(q, "tls:");
		if (endptr) {
			flags |= CONNECT_F_TLS;
			q = endptr;
		}

		if (!string_to_host_or_addr(q, &endptr, &addr)) {
			g_message("Expected hostname or IP address");
			break;
		}

		if (!is_host_addr(addr)) {
			hostname = q;
			hostname_len = endptr - q;
		}

		q = endptr;

		if (':' == *q) {
			guint32 v;
			gint error;

			v = parse_uint32(&q[1], &endptr, 10, &error);
			if (error || 0 == v || v > 0xffff) {
				g_message("Cannot parse port");
				break;
			}

			port = v;
			q = skip_ascii_spaces(endptr);
		} else {
			q = skip_ascii_spaces(endptr);
			if ('\0' != *q && ',' != *q) {
				g_message("Expected \",\" or \":\"");
				break;
			}
		}

		if (!hostname) {
			guc_node_add(addr, port, flags);
		} else {
			struct add_node_context *ctx;
			gchar *p;

			if ('\0' == hostname[hostname_len])	{
				p = NULL;
			} else {
				size_t n = 1 + hostname_len;

				g_assert(n > hostname_len);
				p = g_malloc(n);
				g_strlcpy(p, hostname, n);
				hostname = p;
			}

			ctx = walloc(sizeof *ctx);
			ctx->port = port;
			ctx->flags = flags;
			guc_adns_resolve(hostname, add_node_helper, ctx);

			G_FREE_NULL(p);
		}
	}
}

/* vi: set ts=4 sw=4 cindent: */
