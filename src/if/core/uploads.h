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

#ifndef _if_core_uploads_h_
#define _if_core_uploads_h_

#include "common.h"
#include "lib/host_addr.h"

typedef guint32 gnet_upload_t;

/**
 * Upload states.
 */

typedef enum {
	GTA_UL_PUSH_RECEIVED    = 1,    /**< We got a push request */
	GTA_UL_COMPLETE         = 2,    /**< The file has been sent completely */
	GTA_UL_SENDING          = 3,    /**< We are sending data */
	GTA_UL_HEADERS          = 4,    /**< Receiving the HTTP request headers */
	GTA_UL_WAITING          = 5,    /**< Waiting end of follow-up request */
	GTA_UL_ABORTED          = 6,    /**< Upload removed during operation */
	GTA_UL_CLOSED           = 7,    /**< Upload removed while waiting */
	GTA_UL_QUEUED           = 8,    /**< Upload is queued */
	GTA_UL_QUEUE            = 9,    /**< Send a queue (Similar to push) */
	GTA_UL_QUEUE_WAITING    = 10,   /**< Connect back with GTA_UL_QUEUE was
									     success now waiting for a response */
	GTA_UL_EXPECTING        = 11,   /**< Expecting follow-up HTTP request */
} upload_stage_t;

typedef struct gnet_upload_status {
	upload_stage_t status;
	filesize_t  pos;	 /**< Read position in file we're sending */
	guint32 bps;         /**< Current transfer rate */
	guint32 avg_bps;     /**< Average transfer rate */
	time_t  last_update;
	guint reqnum;		 /**< Count of uploaded chunks */
	guint error_count;	 /**< Number of errors */

	gboolean parq_quick;
	gboolean parq_frozen;
	guint	parq_position;
	guint	parq_size;
	guint32	parq_lifetime;
	guint32	parq_retry;
	guint	parq_queue_no;
} gnet_upload_status_t;

typedef struct gnet_upload_info {
	gnet_upload_t upload_handle;

	host_addr_t addr;		/**< remote IP address */
	host_addr_t gnet_addr;	/**< Advertised Gnutella address for connecting */

	filesize_t file_size;	/**< Size of requested file */
	filesize_t range_start;	/**< First byte to send, inclusive */
	filesize_t range_end;	/**< Last byte to send, inclusive */

	const gchar  *name;		/**< Name of requested file (converted to UTF-8) */
	const gchar  *user_agent;	/**< Remote user agent (converted to UTF-8) */

	time_t  start_date;
	time_t  last_update;

	gboolean push;		/**< Whether we're pushing or not */
	gboolean partial;	/**< Whether it's a partial file */
	gboolean encrypted; /**< Whether the connection is (TLS) encrypted */
	
	guint16 gnet_port;		/**< Advertised Gnutella listening port */
	guint16 country;  		/**< Contry of origin */
} gnet_upload_info_t;

/*
 * State inspection macros.
 */

#define UPLOAD_IS_CONNECTING(u)						\
	(	(u)->status == GTA_UL_HEADERS				\
	||	(u)->status == GTA_UL_PUSH_RECEIVED			\
	||	(u)->status == GTA_UL_QUEUE					\
	||	(u)->status == GTA_UL_QUEUE_WAITING			\
	||	(u)->status == GTA_UL_EXPECTING				\
	||	(u)->status == GTA_UL_WAITING	)

#define UPLOAD_IS_COMPLETE(u)	\
	((u)->status == GTA_UL_COMPLETE)

#define UPLOAD_IS_SENDING(u)	\
	((u)->status == GTA_UL_SENDING)

#define UPLOAD_IS_QUEUED(u)	\
	((u)->status == GTA_UL_QUEUED)

#define UPLOAD_WAITING_FOLLOWUP(u) \
	((u)->status == GTA_UL_WAITING || (u)->status == GTA_UL_EXPECTING)

#define UPLOAD_READING_HEADERS(u) \
	((u)->status == GTA_UL_HEADERS || (u)->status == GTA_UL_WAITING)

struct ul_stats {
	const char  *pathname;	/**< Atom, (from sf->pathname) */
	const char  *filename;	/**< Atom, UTF-8 (from sf->name_nfc) */
	filesize_t size;
	guint32 attempts;
	guint32 complete;
	time_t rtime;		/**< time of last request */
	time_t dtime;		/**< time of last downloaded bytes */
	guint64 bytes_sent;
	double norm;		/**< bytes sent / file size */
	const struct sha1 *sha1;	/**< SHA1 of file (atom), if known, or NULL */
	void *user_data;	/**< Used by the GUI side to store context */
};

/*
 * Uploads callback definitions
 */
typedef void (*upload_added_listener_t)(gnet_upload_t);
typedef void (*upload_removed_listener_t)(gnet_upload_t, const gchar *);
typedef void (*upload_info_changed_listener_t)(gnet_upload_t);

#define upload_add_listener(signal, callback) \
    CAT3(upload_add_,signal,_listener)(callback);

#define upload_remove_listener(signal, callback) \
    CAT3(upload_remove_,signal,_listener)(callback);

/*
 * Public interface, visible from the bridge.
 */

#ifdef CORE_SOURCES

gnet_upload_info_t *upload_get_info(gnet_upload_t);
void upload_free_info(gnet_upload_info_t *);
void upload_get_status(gnet_upload_t, gnet_upload_status_t *);
void upload_kill_addr(const host_addr_t);
void upload_add_upload_added_listener(upload_added_listener_t);
void upload_remove_upload_added_listener(upload_added_listener_t);
void upload_add_upload_removed_listener(upload_removed_listener_t);
void upload_remove_upload_removed_listener(upload_removed_listener_t);
void upload_add_upload_info_changed_listener(upload_info_changed_listener_t);
void upload_remove_upload_info_changed_listener(upload_info_changed_listener_t);
void upload_kill(gnet_upload_t);
void upload_stats_prune_nonexistent(void);

#endif /* CORE_SOURCES */
#endif /* _if_core_uploads_h_ */

/* vi: set ts=4 sw=4 cindent: */
