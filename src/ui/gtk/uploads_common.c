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

RCSID("$Id$");

#include "uploads.h"			/* For upload_row_data_t */
#include "uploads_common.h"
#include "search_common.h"
#include "settings.h"

#include "if/gui_property.h"
#include "if/gnet_property.h"
#include "if/core/uploads.h"
#include "if/bridge/ui2c.h"

#include "lib/host_addr.h"
#include "lib/misc.h"
#include "lib/glib-missing.h"	/* For gm_snprintf() */
#include "lib/tm.h"
#include "lib/override.h"		/* Must be the last header included */

#define IO_STALLED		60		/**< If nothing exchanged after that many secs */

/**
 * Invoked from the core when we discover the Gnutella address and port
 * of the uploading party.
 */
void
uploads_gui_set_gnet_addr(gnet_upload_t u, host_addr_t addr, guint16 port)
{
	upload_row_data_t *rd = uploads_gui_get_row_data(u);

	if (rd != NULL) {
		rd->gnet_addr = addr;
		rd->gnet_port = port;
	}
}

/**
 *
 * @returns a floating point value from [0:1] which indicates
 * the total progress of the upload.
 */
gfloat
uploads_gui_progress(const gnet_upload_status_t *u,
	const upload_row_data_t *data)
{
	gfloat progress = 0.0;
	filesize_t requested;

	if (u->pos < data->range_start) /* No progress yet */
		return 0.0;

	switch (u->status) {
    case GTA_UL_HEADERS:
    case GTA_UL_WAITING:
    case GTA_UL_PFSP_WAITING:
    case GTA_UL_ABORTED:
	case GTA_UL_QUEUED:
    case GTA_UL_QUEUE:
    case GTA_UL_QUEUE_WAITING:
	case GTA_UL_PUSH_RECEIVED:
		progress = 0.0;
		break;
    case GTA_UL_CLOSED:
	case GTA_UL_COMPLETE:
		progress = 1.0;
		break;
	case GTA_UL_SENDING:
		requested = data->range_end - data->range_start + 1;
		if (requested != 0) {
			/*
			 * position divided by 1 percentage point, found by dividing
			 * the total size by 100
			 */
			progress = (gfloat) (u->pos - data->range_start) / requested;
		} else {
			progress = 0.0;
		}
		break;
	}
	return progress;
}

/**
 * @return a pointer to a static buffer containing a string which
 * describes the current status of the upload.
 */
const gchar *
uploads_gui_status_str(const gnet_upload_status_t *u,
	const upload_row_data_t *data)
{
	static gchar tmpstr[256];

	if (u->pos < data->range_start)
		return _("No output yet..."); /* Never wrote anything yet */

    switch (u->status) {
    case GTA_UL_PUSH_RECEIVED:
        return _("Got push, connecting back...");

    case GTA_UL_COMPLETE:
		{
			gint t = delta_time(u->last_update, data->start_date);
	        filesize_t requested = data->range_end - data->range_start + 1;

			gm_snprintf(tmpstr, sizeof(tmpstr),
				_("%sCompleted (%s) %s"),
				u->parq_quick ? "* " : "",
				t > 0 ? short_rate(requested / t, show_metric_units())
						: _("< 1s"),
				t > 0 ? short_time(t) : "");
		}
        break;

    case GTA_UL_SENDING:
		{
			/* Time Remaining at the current rate, in seconds  */
			filesize_t tr = (data->range_end + 1 - u->pos) / MAX(1, u->avg_bps);
			gdouble p = uploads_gui_progress(u, data);
			time_t now = tm_time();
			gboolean stalled = delta_time(now, u->last_update) > IO_STALLED;
			gchar pbuf[32];

			gm_snprintf(pbuf, sizeof pbuf, "%5.02f%% ", p * 100.0);
			gm_snprintf(tmpstr, sizeof tmpstr, _("%s%s(%s) TR: %s"),
				u->parq_quick ? "* " : "",
				p > 1.0 ? pbuf : "",
				stalled ? _("stalled")
					: short_rate(u->bps, show_metric_units()),
				short_time(tr));
		}
		break;

    case GTA_UL_HEADERS:
        return _("Waiting for headers...");

    case GTA_UL_WAITING:
        return _("Waiting for further request...");

    case GTA_UL_PFSP_WAITING:
        return _("Unavailable range, waiting retry...");

    case GTA_UL_ABORTED:
        return _("Transmission aborted");

    case GTA_UL_CLOSED:
        return _("Transmission complete");

	case GTA_UL_QUEUED:
		{
			guint32 max_up, cur_up;
			gboolean queued;
			gchar tbuf[64];

			/*
			 * Status: GTA_UL_QUEUED. When PARQ is enabled, and all upload
			 * slots are full an upload is placed into the PARQ-upload. Clients
			 * supporting Queue 0.1 and 1.0 will get an active slot. We
			 * probably want to display this information
			 *		-- JA, 06/02/2003
			 */


			gnet_prop_get_guint32_val(PROP_MAX_UPLOADS, &max_up);
			gnet_prop_get_guint32_val(PROP_UL_RUNNING, &cur_up);
			queued = u->parq_position > max_up - cur_up;

			/* position 1 should always get an upload slot */
			if (u->parq_retry > 0) {
				gm_snprintf(tbuf, sizeof tbuf,
							" %s,", short_time(u->parq_retry));
			} else {
				tbuf[0] = '\0';
			}
			gm_snprintf(tmpstr, sizeof tmpstr,
						_("%s [%d] (slot %d/%d)%s %s %s"),
						queued ? _("Queued") : _("Waiting"),
						u->parq_queue_no,
						u->parq_position,
						u->parq_size,
						tbuf,
						_("lifetime:"),
						short_time(u->parq_lifetime));

		}
		break;

    case GTA_UL_QUEUE:
        /*
         * PARQ wants to inform a client that action from the client its side
         * is wanted. So it is trying to connect back.
         *      -- JA, 15/04/2003
         */
        return _("Sending QUEUE, connecting back...");

    case GTA_UL_QUEUE_WAITING:
        /*
         * PARQ made a connect back because some action from the client is
         * wanted. The connection is established and now waiting for some action
         *      -- JA, 15/04/2003
         */
		return _("Sent QUEUE, waiting for headers...");
	}

    return tmpstr;
}

/**
 * @return whether the entry for the upload `ul' should be removed
 * from the UI with respect to the configured behaviour.
 */
gboolean
upload_should_remove(time_t now, const upload_row_data_t *ul)
{
	property_t prop = 0;

	g_assert(NULL != ul);

	switch (ul->status) {
	case GTA_UL_COMPLETE:
		prop = PROP_AUTOCLEAR_COMPLETED_UPLOADS;
		break;
	case GTA_UL_CLOSED:
	case GTA_UL_ABORTED:
		prop = PROP_AUTOCLEAR_FAILED_UPLOADS;
		break;
	case GTA_UL_PUSH_RECEIVED:
	case GTA_UL_SENDING:
	case GTA_UL_HEADERS:
	case GTA_UL_WAITING:
	case GTA_UL_QUEUED:
	case GTA_UL_QUEUE:
	case GTA_UL_QUEUE_WAITING:
	case GTA_UL_PFSP_WAITING:
		break;
	}

	if (0 != prop) {
		guint32 grace;

		gnet_prop_get_guint32_val(PROP_ENTRY_REMOVAL_TIMEOUT, &grace);
		if (delta_time(now, ul->last_update) > grace) {
			gboolean val;

			gui_prop_get_boolean_val(prop, &val);
			return val;
		}
	}

	return FALSE;
}

/**
 * @return A pointer to a static buffer holding the host address as string.
 */
const gchar * 
uploads_gui_host_string(const gnet_upload_info_t *u)
{
	static gchar buf[MAX_HOSTLEN + sizeof " (E)"];

	concat_strings(buf, sizeof buf,
		host_addr_to_string(u->addr), u->encrypted ? " (E)" : "", (void *) 0);
	return buf;
}

/**
 * Initiate a browse host of the uploading host.
 */
void
uploads_gui_browse_host(host_addr_t addr, guint16 port)
{
	if (host_addr_is_routable(addr) && port != 0)
		search_gui_new_browse_host(NULL, addr, port, NULL, FALSE, NULL);
}

/* vi: set ts=4 sw=4 cindent: */
