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
 * Message queues, writing to a TCP stack.
 *
 * @author Raphael Manfredi
 * @date 2002-2003
 */

#include "common.h"

RCSID("$Id$")

#include "nodes.h"
#include "mq.h"
#include "mq_tcp.h"
#include "gmsg.h"
#include "tx.h"
#include "gnet_stats.h"

#include "lib/pmsg.h"
#include "lib/walloc.h"

#include "if/gnet_property_priv.h"
#include "if/core/main.h"

#include "lib/override.h"		/* Must be the last header included */

#define MQ_MAXIOV		256		/**< Our limit on the I/O vectors we build */
#define MQ_MINIOV		2		/**< Minimum amount of I/O vectors in service */
#define MQ_MINSEND		256		/**< Minimum size we try to send */

static void mq_tcp_service(gpointer data);
static void mq_tcp_putq(mqueue_t *q, pmsg_t *mb);
static const struct mq_ops mq_tcp_ops = {
	mq_tcp_putq,			/* putq */
};


/**
 * Create new message queue capable of holding `maxsize' bytes, and
 * owned by the supplied node.
 */
mqueue_t *
mq_tcp_make(int maxsize, struct gnutella_node *n, struct txdriver *nd)
{
	mqueue_t *q;

	q = walloc0(sizeof *q);

	q->magic = MQ_MAGIC;
	q->node = n;
	q->tx_drv = nd;
	q->maxsize = maxsize;
	q->lowat = maxsize >> 2;		/* 25% of max size */
	q->hiwat = maxsize >> 1;		/* 50% of max size */
	q->qwait = slist_new();
	q->ops = &mq_tcp_ops;
	q->cops = mq_get_cops();

	tx_srv_register(nd, mq_tcp_service, q);

	return q;
}

/**
 * Service routine for TCP message queue.
 */
static void
mq_tcp_service(gpointer data)
{
	mqueue_t *q = (mqueue_t *) data;
	static struct iovec iov[MQ_MAXIOV];
	int iovsize;
	int iovcnt;
	int sent;
	ssize_t r;
	GList *l;
	int dropped;
	int maxsize;
	gboolean saturated;
	gboolean has_prioritary = FALSE;

again:
	mq_check(q, 0);
	g_assert(q->count);		/* Queue is serviced, we must have something */

	iovcnt = 0;
	sent = 0;
	dropped = 0;

	/*
	 * Build I/O vector.
	 *
	 * Optimize our time: don't spend time building too much if we're
	 * not likely to send anything.  We limit to 1.5 times the amount we
	 * last wrote last time we were called, with a minimum of 2 entries.
	 */

	iovsize = MIN(MQ_MAXIOV, q->count);
	maxsize = q->last_written + (q->last_written >> 1);		/* 1.5 times */
	maxsize = MAX(MQ_MINSEND, maxsize);

	for (l = q->qtail; l && iovsize > 0; /* empty */) {
		struct iovec *ie;
		pmsg_t *mb = (pmsg_t *) l->data;
		char *mbs = pmsg_start(mb);

		/*
		 * Don't build too much.
		 */

		if (iovcnt > MQ_MINIOV && maxsize < 0)
			break;

		/*
		 * Honour hops-flow, and ensure there is a route for possible replies.
		 */

		if (pmsg_check(mb, q)) {
			/* send the message */
			l = g_list_previous(l);
			iovsize--;
			ie = &iov[iovcnt++];
			ie->iov_base = deconstify_gpointer(mb->m_rptr);
			ie->iov_len = pmsg_size(mb);
			maxsize -= ie->iov_len;
			if (pmsg_prio(mb))
				has_prioritary = TRUE;
		} else {
			gnet_stats_count_flowc(mbs);	/* Done before message freed */
			if (q->qlink)
				q->cops->qlink_remove(q, l);

			/* drop the message, will be freed by mq_rmlink_prev() */
			l = q->cops->rmlink_prev(q, l, pmsg_size(mb));

			dropped++;
		}
	}

	mq_check(q, 0);
	g_assert(iovcnt > 0 || dropped > 0);

	if (dropped > 0)
		node_add_txdrop(q->node, dropped);	/* Dropped during TX */

	if (iovcnt == 0)						/* Nothing to send */
		goto update_servicing;

	/*
	 * Write as much as possible.
	 */

	if (has_prioritary)
		node_flushq(q->node);

	r = tx_writev(q->tx_drv, iov, iovcnt);

	g_assert((ssize_t) -1 == r || !tx_has_error(q->tx_drv));

	if ((ssize_t) -1 == r || r == 0) {
		q->last_written = 0;
		if (r == 0)
			goto update_servicing;
		return;
	}

	if (has_prioritary) {
		tx_flush(q->tx_drv);
		if (tx_has_error(q->tx_drv))
			return;
	}

	node_add_tx_given(q->node, r);
	q->last_written = r;

	if (q->flags & MQ_FLOWC)
		q->flowc_written += r;

	/*
	 * Determine which messages we wrote, and whether we saturated the
	 * lower layer.
	 */

	iovsize = iovcnt;
	iovcnt = 0;
	saturated = FALSE;

	for (l = q->qtail; l && r > 0 && iovsize > 0; iovsize--) {
		struct iovec *ie = &iov[iovcnt++];
		pmsg_t *mb = (pmsg_t *) l->data;

		if ((guint) r >= ie->iov_len) {			/* Completely written */
			char *mb_start = pmsg_start(mb);
			guint8 function = gmsg_function(mb_start);
			sent++;
			pmsg_mark_sent(mb);
            gnet_stats_count_sent(q->node,
				function, gmsg_hops(mb_start), pmsg_size(mb));
			switch (function) {
			case GTA_MSG_SEARCH:
				node_inc_tx_query(q->node);
				break;
			case GTA_MSG_SEARCH_RESULTS:
				node_inc_tx_qhit(q->node);
				break;
			default:
				break;
			}
			r -= ie->iov_len;
			if (q->qlink)
				q->cops->qlink_remove(q, l);
			l = q->cops->rmlink_prev(q, l, ie->iov_len);
		} else {
			g_assert(r > 0 && r < pmsg_size(mb));
			g_assert(r < q->size);
			mb->m_rptr += r;
			q->size -= r;
			g_assert(l == q->qtail);	/* Partially written, is at tail */
			saturated = TRUE;
			break;
		}
	}

	mq_check(q, 0);
	g_assert(r == 0 || iovsize > 0);
	g_assert(q->size >= 0 && q->count >= 0);

	if (sent)
		node_add_sent(q->node, sent);

	/*
	 * We're in the service routine, and we need to flush as much as possible
	 * to the lower layer.  If it has not saturated yet, continue.  This is
	 * the only way to ensure the lower layer will keep calling our service
	 * routine.  For instance, the compressing layer will only invoke us when
	 * it has flushed its pending buffer and it was in flow control.
	 *
	 *		--RAM. 01/03/2003
	 */

	if (!saturated && q->count > 0)
		goto again;

update_servicing:
	/*
	 * Update flow-control information.
	 */

	q->cops->update_flowc(q);

	/*
	 * If queue is empty, disable servicing.
	 *
	 * If not, we've been through a writing cycle and there are still some
	 * data we could not send.  We know the TCP window is full, or we
	 * would not be servicing and still get some data to send.  Notify
	 * the node.
	 *		--RAM, 15/03/2002
	 */

	if (q->size == 0) {
		g_assert(q->count == 0);
		tx_srv_disable(q->tx_drv);
		node_tx_service(q->node, FALSE);
	} else
		node_flushq(q->node);		/* Need to flush kernel buffers faster */
}

/**
 * Enqueue message, which becomes owned by the queue.
 */
static void
mq_tcp_putq(mqueue_t *q, pmsg_t *mb)
{
	int size;				/* Message size */
	char *mbs;				/* Start of message */
	guint8 function;		/* Gnutella message function */
	guint8 hops;			/* Gnutella message hop count */
	gboolean prioritary;	/* Is message prioritary? */
	gboolean error = FALSE;

again:
	g_assert(q);
	g_assert(!pmsg_was_sent(mb));
	g_assert(pmsg_is_unread(mb));
	mq_check(q, 0);

	size = pmsg_size(mb);

	if (size == 0) {
		g_warning("mq_putq: called with empty message");
		goto cleanup;
	}

	if (q->flags & MQ_DISCARD) {
		g_warning("mq_putq: called whilst queue shutdown");
		goto cleanup;
	}

	/*
	 * Protect against recursion: we must not invoke puthere() whilst in
	 * the middle of another putq() or we would corrupt the qlink array:
	 * Messages received during recursion are inserted into the qwait list
	 * and will be stuffed back into the queue when the initial putq() ends.
	 *		--RAM, 2006-12-29
	 */

	if (q->putq_entered > 0) {
		if (debugging(20))
			g_warning(
				"mq_tcp_putq: %s recursion detected (%d already pending)",
				mq_info(q), slist_length(q->qwait));
		slist_append(q->qwait, mb);
		return;
	}

	q->putq_entered++;
	
	mbs = pmsg_start(mb);
	function = gmsg_function(mbs);
	hops = gmsg_hops(mbs);
	prioritary = pmsg_prio(mb) != PMSG_P_DATA;

	gnet_stats_count_queued(q->node, function, hops, size);

	/*
	 * If queue is empty, attempt a write immediatly.
	 */

	if (q->qhead == NULL) {
		ssize_t written;

		if (pmsg_check(mb, q)) {
			if (prioritary)
				node_flushq(q->node);

			written = tx_write(q->tx_drv, mbs, size);

			/*
			 * If that assertion fails, then it means there is an error
			 * in the TX stack which set tx_has_error() even though it
			 * reports data written and not -1.
			 *		--RAM, 2006-12-29
			 */
			g_assert((ssize_t) -1 == written || !tx_has_error(q->tx_drv));

			if ((ssize_t) -1 == written) {
				error = TRUE;
				goto cleanup;
			}

			if (prioritary && written == size) {
				tx_flush(q->tx_drv);
				if (tx_has_error(q->tx_drv)) {
					error = TRUE;
					goto cleanup;
				}
			}
		} else {
			gnet_stats_count_flowc(mbs);
			node_inc_txdrop(q->node);		/* Dropped during TX */
			written = -1;
		}

		if (written < 0)
			goto cleanup;			/* Node already removed if necessary */

		node_add_tx_given(q->node, written);

		if (written == size) {
			pmsg_mark_sent(mb);
			node_inc_sent(q->node);
            gnet_stats_count_sent(q->node, function, hops, size);
			switch (function) {
			case GTA_MSG_SEARCH:
				node_inc_tx_query(q->node);
				break;
			case GTA_MSG_SEARCH_RESULTS:
				node_inc_tx_qhit(q->node);
				break;
			default:
				break;
			}
			goto cleanup;
		}

		mb->m_rptr += written;		/* Partially written */
		size -= written;

		/* FALL THROUGH */
	}

	/*
	 * Enqueue message.
	 */

	q->cops->puthere(q, mb, size);
	mb = NULL;

cleanup:
	if (mb) {
		pmsg_free(mb);
		mb = NULL;
	}
	g_assert(q->putq_entered >= 0);

	/*
	 * When reaching that point with a zero putq_entered counter, it means
	 * we triggered an early error condition.  Bail out.
	 */

	if (q->putq_entered == 0)
		error = TRUE;
	else
		q->putq_entered--;

	mq_check(q, 0);

	/*
	 * If we're exiting here with no other putq() registered, then we must
	 * pop an item off the head of the list and iterate again.  We stop as
	 * soon as a write error is reported by the TX stack.
	 */

	if (0 == q->putq_entered && !error) {
		mb = slist_shift(q->qwait);
		if (mb) {
			if (debugging(20))
				g_warning(
					"mq_tcp_putq: %s flushing waiting (%d still pending)",
					mq_info(q), slist_length(q->qwait));
			goto again;
		}
	}

	return;
}

/* vi: set ts=4 sw=4 cindent: */
