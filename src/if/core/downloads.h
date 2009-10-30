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

#ifndef _if_core_downloads_h_
#define _if_core_downloads_h_

#include "lib/tm.h"				/* For tm_t */
#include "lib/event.h"			/* For frequency_t */
#include "lib/hashlist.h"
#include "lib/list.h"
#include "lib/slist.h"

#include "core/pproxy.h"
#include "core/rx.h"
#include "core/rx_link.h"
#include "core/rx_chunk.h"

#include "if/core/search.h"		/* For gnet_host_vec_t */

/***
 *** Sources (traditionally called "downloads")
 ***/

typedef guint32 gnet_src_t;

#define URN_INDEX	0xffffffff		/**< Marking index, indicates URN instead */

/*
 * We keep a list of all the downloads queued per GUID+IP:port (host).  Indeed
 * some broken clients (e.g. Morpheus) share the same GUID, so we cannot
 * fully discriminate on the GUID alone.  So GUID+IP:port forms the "key",
 * the `dl_key' structure.
 *
 * Inside the `dl_server', we keep track all `download' structures and
 * other server-related information, which are shared by all downloads
 * from this host..
 *
 * Within a single server, a download can be in either runnning, waiting
 * or stopped.  An array of lists is kept, and since the download can be
 * in only one of them, it also keeps track of the proper list index.
 */

enum dl_list {
	DL_LIST_INVALID	= -1,
	DL_LIST_RUNNING	= 0,
	DL_LIST_WAITING = 1,
	DL_LIST_STOPPED = 2,
	DL_LIST_SZ		= 3
};

struct vernum {
	guint major;
	guint minor;
};

struct guid;

struct dl_key {
	const struct guid *guid;	/**< GUID of server (atom) */
	host_addr_t addr;			/**< IP address of server */
	guint16 port;				/**< Port of server */
};

enum dl_server_magic { DL_SERVER_MAGIC = 0x5e45e4ffU };

struct dl_server {
	enum dl_server_magic magic;	/**< Magic number */
	int refcnt;					/**< Reference count */
	struct dl_key *key;			/**< Key properties */
	list_t *list[DL_LIST_SZ];	/**< Download lists */
	const char *vendor;			/**< Remote server vendor string (atom) */
	const char *hostname;		/**< Remote hostname, if known (atom) */
	pproxy_set_t *proxies;		/**< Known push proxies */
	GHashTable *sha1_counts;
	time_t retry_after;		/**< Time at which we may retry from this host */
	time_t dns_lookup;		/**< Last DNS lookup for hostname */
	time_t last_connect;	/**< When we last connected to that server */
	struct vernum parq_version; /**< Supported queueing version */
	guint speed_avg;			/**< Average (EMA) upload speed, in bytes/sec */
	guint32 attrs;
	guint16 country;			/**< Country of origin -- encoded ISO3166 */
};

static inline gboolean
dl_server_valid(const struct dl_server *s)
{
	return s != NULL && s->magic == DL_SERVER_MAGIC;
}

/**
 * Download states.
 */

typedef enum {
    GTA_DL_INVALID,			/**< Never used */
    GTA_DL_QUEUED,			/**< Download queued, will start later */
    GTA_DL_CONNECTING,  	/**< We are connecting to the server */
    GTA_DL_PUSH_SENT,		/**< Sent a push, waiting connection */
    GTA_DL_FALLBACK,		/**< Direct request failed, using push */
    GTA_DL_REQ_SENT,		/**< Request sent, waiting for HTTP headers */
    GTA_DL_HEADERS,			/**< We are receiving the HTTP headers */
    GTA_DL_RECEIVING,		/**< We are receiving the data of the file */
    GTA_DL_COMPLETED,		/**< Download is completed */
    GTA_DL_ERROR,			/**< Download is stopped due to error */
    GTA_DL_ABORTED,			/**< User used the 'Abort Download' button */
    GTA_DL_TIMEOUT_WAIT,	/**< Waiting to try connecting again */
    GTA_DL_REMOVED,			/**< Download was removed, pending free */
    GTA_DL_VERIFY_WAIT,		/**< Waiting to verify SHA1 */
    GTA_DL_VERIFYING,		/**< Computing SHA1 */
    GTA_DL_VERIFIED,		/**< Verify of SHA1 done */
    GTA_DL_MOVE_WAIT,		/**< Waiting to be moved to "done/bad" dir */
    GTA_DL_MOVING,			/**< Being moved to "done/bad" dir */
    GTA_DL_DONE,			/**< All done! */
    GTA_DL_SINKING,			/**< Sinking HTML reply */
    GTA_DL_ACTIVE_QUEUED,	/**< Actively queued */
    GTA_DL_PASSIVE_QUEUED,	/**< Passively queued */
    GTA_DL_REQ_SENDING,		/**< Sending HTTP request */
	GTA_DL_IGNORING			/**< Ignoring received data on resuming mismatch */
} download_status_t;

typedef struct download download_t;

struct bio_source;
struct http_buffer;

enum dl_bufmode {
	DL_BUF_READING,
	DL_BUF_WRITING
};

/**
 * Structure used to control read buffering for active downloads.
 * Each buffer in this pool is of SOCK_BUFSZ bytes, and the first
 * buffer is always the buffer from the socket structure.
 */
struct dl_buffers {
	enum dl_bufmode mode;	/**< I/O vector mode */
	slist_t *list;			/**< List of pmsg_t items */
	size_t amount;			/**< Amount to buffer (extra is read-ahead) */
	size_t held;			/**< Amount of data held in read buffers */
};

struct file_object;

enum download_magic { DOWNLOAD_MAGIC = 0x2dd6efe9 };	/**< Magic number */

struct download {
	enum download_magic magic;	/**< Magic number */
    gnet_src_t src_handle;      /**< Handle */
	gboolean src_handle_valid;	/**< TRUE if src_handle is initialized */

	char error_str[256];		/**< Used to snprintf() error strings */
	download_status_t status;   /**< Current status of the download */
	gpointer io_opaque;			/**< Opaque I/O callback information */
	rxdrv_t *rx;					/**< RX stack top */

	struct bio_source *bio;		/**< Bandwidth-limited source */

	struct dl_server *server;	/**< Remote server description */
	enum dl_list list_idx;		/**< List to which download belongs in server */

	struct dl_file_info *file_info;
	guint32 record_index;		/**< Index of the file on the Gnutella server */
	const char *file_name;		/**< Name of the file on the Gnutella server */
	filesize_t file_size;		/**< Total size of the file, in bytes */

	filesize_t size;			/**< Total size of the next request, in bytes */
	filesize_t skip;			/**< # of bytes for file we had before start */
	filesize_t pos;				/**< # of bytes of the file we currently have */
	filesize_t range_end;		/**< 1st byte offset AFTER requested range */

	struct gnutella_socket *socket;
	struct file_object *out_file;	/**< downloaded file */
	guint32 overlap_size;		/**< Size of the overlapping window on resume */
	struct http_buffer *req;	/**< HTTP request, when partially sent */
	struct dl_buffers *buffers;	/**< Buffers for reading, only when active */

	time_t start_date;			/**< Download start date */
	time_t last_update;			/**< Last status update or I/O */
	time_t last_gui_update;		/**< Last stats update on the GUI */
	time_t record_stamp;		/**< Stamp of the query hit that launched us */
	time_t retry_after;			/**< Time at which we may retry this download */
	time_t head_ping_sent;		/**< Time at which last HEAD ping was sent */
	tm_t header_sent;			/**< When we sent headers, for latency */

	guint32 retries;
	guint32 timeout_delay;
	guint32 served_reqs;		/**< Amount of served requests on connection */
	guint32 mismatches;			/**< Amount of resuming data mismatches */
	guint32 header_read_eof;	/**< EOF errors with empty headers */
	guint32 data_timeouts;		/**< # of timeouts after getting headers */

	const char *remove_msg;

	const struct sha1 *sha1;	/**< Known SHA1 (binary atom), NULL if none */
	const char *uri;			/**< Uri if not dealing with regular gnutella
								 **< file download */
	time_t last_dmesh;			/**< Time when last download mesh was sent */

	GSList *ranges;				/**< PFSP -- known list of ranges, or NULL */
	filesize_t ranges_size;		/**< PFSP -- size of remotely available data */
	filesize_t sinkleft;		/**< Amount of data left to sink */

	guint32 flags;
	guint32 cflags;

	gboolean keep_alive;		/**< Keep HTTP connection? */
	gboolean push;				/**< Currently in push mode */
	gboolean always_push;		/**< Always use the push method for this */
	gboolean got_giv;			/**< Whether initiated from GIV reception */
	gboolean unavailable;		/**< Set on Timout, Push route lost */

	struct cproxy *cproxy;		/**< Push proxy being used currently */
	struct parq_dl_queued *parq_dl;	/**< Queuing status */
	struct browse_ctx *browse;	/**< For browse-host requests */
	struct thex_download *thex;	/**< For THEX downloads */
};

/*
 * Download flags.
 */

enum {
	DL_F_PARTIAL		= 1 << 28,	/**< Partial source */
	DL_F_FROM_ERROR		= 1 << 27,	/**< Switched after error occurred */
	DL_F_FROM_PLAIN		= 1 << 26,	/**< Switched from plain file download */
	DL_F_SWITCHED		= 1 << 25,	/**< Scheduled after resource switching */
	DL_F_MUST_IGNORE	= 1 << 24,	/**< Must ignore data when they come */
	DL_F_FAKE_G2		= 1 << 23,	/**< Trying to fake G2, intuition only */
	DL_F_TRIED_TLS		= 1 << 22,	/**< TLS connection was tried already */
	DL_F_TRY_TLS		= 1 << 21,	/**< Try to initiate a TLS connection */
	DL_F_GOT_TTH		= 1 << 20,	/**< Tigertree fetching finished */
	DL_F_FETCH_TTH		= 1 << 19,	/**< Tigertree data is being fetched */
	DL_F_UDP_PUSH		= 1 << 18,	/**< UDP push already attempted */
	DL_F_THEX			= 1 << 17,	/**< THEX download */
	DL_F_PAUSED			= 1 << 16,	/**< Paused by user */
	DL_F_INITIAL		= 1 << 15,	/**< First request on this connection */
	DL_F_PREFIX_HEAD	= 1 << 14,	/**< Sent HEAD request before GET */
	DL_F_MARK			= 1 << 13,	/**< Marked in traversal */
	DL_F_SUSPENDED		= 1 << 12,	/**< Suspended, do not schedule */
	DL_F_TRANSIENT		= 1 << 11,	/**< Transient, don't persist */
	DL_F_BROWSE			= 1 << 10,	/**< Browse host type (requests "/") */
	DL_F_DNS_LOOKUP		= 1 << 9,	/**< Attempted DNS lookup */
	DL_F_PASSIVE_QUEUED	= 1 << 8,	/**< Download is passively queued */
	DL_F_ACTIVE_QUEUED	= 1 << 7,	/**< Download is actively queued */
	DL_F_SUNK_DATA		= 1 << 6,	/**< Whether we previously sunk data */
	DL_F_SHRUNK_REPLY	= 1 << 5,	/**< Server sending less than we asked */
	DL_F_CHUNK_CHOSEN	= 1 << 4,	/**< Retrying with specific chunk */
	DL_F_REPLIED		= 1 << 3,	/**< Servent replied to last request */
	DL_F_OVERLAPPED		= 1 << 2,	/**< We went through overlap checking */
	DL_F_PUSH_IGN		= 1 << 1,	/**< Trying to ignore push flag */
	DL_F_URIRES			= 1 << 0	/**< We sent a "/uri-res/N2R?" request */
};

/*
 * Server attributes.
 */
enum {
	DLS_A_TLS			= 1 << 11,	/**< Server known to support TLS */
	DLS_A_DHT_PROX		= 1 << 10,	/**< Looking for push-proxies in DHT */
	DLS_A_G2_ONLY		= 1 << 9,	/**< Server is "G2" only */
	DLS_A_FOOBAR		= 1 << 8,	/**< Server is foobar */
	DLS_A_REMOVED		= 1 << 7,	/**< Server marked for removal */
	DLS_A_DNS_LOOKUP	= 1 << 6,	/**< Perform DNS lookup if possible */
	DLS_A_BANNING		= 1 << 5,	/**< Server might be banning us */
	DLS_A_MINIMAL_HTTP	= 1 << 4,	/**< Use minimalist HTTP with server */
	DLS_A_NO_HTTP_1_1	= 1 << 3,	/**< Server does NOT support HTTP/1.1 */
	DLS_A_FAKE_G2		= 1 << 2,	/**< Fake a G2 server */
	DLS_A_PUSH_IGN		= 1 << 1,	/**< Ignore pushes, connect directly */
	DLS_A_FAKED_VENDOR	= 1 << 0	/**< Server's vendor was faked */
};

/*
 * Access macros.
 */

#define download_guid(d)		((d)->server->key->guid)
#define download_addr(d)		((d)->server->key->addr)
#define download_port(d)		((d)->server->key->port)
#define download_vendor(d)		((d)->server->vendor)
#define download_country(d)		((d)->server->country)
#define download_hostname(d)	((d)->server->hostname)

#define download_vendor_str(d) \
	((d)->server->vendor ? (d)->server->vendor : "")

#define download_filesize(d)	((d)->file_info->size)
#define download_filedone(d)	((d)->file_info->done + (d)->file_info->buffered)
#define download_fileremain(d)	(download_filesize(d) - download_filedone(d))
#define download_buffered(d)	((d)->buffers == NULL ? 0 : (d)->buffers->held)

/*
 * Sorted list of http_range_t objects, telling us about the available ranges
 * on the remote size, in case the file is partial.
 */
#define download_ranges(d)		((d)->ranges)
#define download_ranges_size(d)	((d)->ranges_size)

/*
 * State inspection macros.
 */

#define DOWNLOAD_IS_QUEUED(d) \
	(  (d)->status == GTA_DL_QUEUED		\
	|| (d)->status == GTA_DL_PASSIVE_QUEUED)

#define DOWNLOAD_IS_PARQED(d) \
	(  (d)->status == GTA_DL_PASSIVE_QUEUED	\
	|| (d)->status == GTA_DL_ACTIVE_QUEUED	 )

#define DOWNLOAD_IS_VERIFYING(d)		 \
	(  (d)->status == GTA_DL_VERIFY_WAIT \
	|| (d)->status == GTA_DL_VERIFYING	 \
	|| (d)->status == GTA_DL_VERIFIED	 )

#define DOWNLOAD_IS_MOVING(d)		 	\
	(  (d)->status == GTA_DL_MOVE_WAIT	\
	|| (d)->status == GTA_DL_MOVING		)

#define DOWNLOAD_IS_STOPPED(d)			\
	(  (d)->status == GTA_DL_ABORTED	\
	|| (d)->status == GTA_DL_ERROR		\
	|| (d)->status == GTA_DL_COMPLETED  \
	|| DOWNLOAD_IS_VERIFYING(d)         \
	|| DOWNLOAD_IS_MOVING(d)            \
	|| (d)->status == GTA_DL_DONE       )

#define DOWNLOAD_IS_ACTIVE(d)			\
	(  (d)->status == GTA_DL_RECEIVING	\
	|| (d)->status == GTA_DL_IGNORING	)

#define DOWNLOAD_IS_WAITING(d)			\
	(  (d)->status == GTA_DL_TIMEOUT_WAIT)

#define DOWNLOAD_IS_ESTABLISHING(d)		\
	(  (d)->status == GTA_DL_CONNECTING \
	|| (d)->status == GTA_DL_PUSH_SENT	\
	|| (d)->status == GTA_DL_FALLBACK	\
	|| (d)->status == GTA_DL_REQ_SENT	\
	|| (d)->status == GTA_DL_REQ_SENDING	\
	|| (d)->status == GTA_DL_ACTIVE_QUEUED	\
	|| (d)->status == GTA_DL_SINKING	\
	|| (d)->status == GTA_DL_IGNORING	\
	|| (d)->status == GTA_DL_HEADERS	)

#define DOWNLOAD_IS_EXPECTING_GIV(d)	\
	(  (d)->status == GTA_DL_PUSH_SENT	\
	|| (d)->status == GTA_DL_FALLBACK	)

#define DOWNLOAD_IS_RUNNING(d)			\
	(	DOWNLOAD_IS_ACTIVE(d)			\
	||	DOWNLOAD_IS_ESTABLISHING(d)		)

#define DOWNLOAD_IS_SWITCHABLE(d)		\
	(  (d)->status == GTA_DL_TIMEOUT_WAIT \
	|| (d)->status == GTA_DL_QUEUED	\
	|| (d)->status == GTA_DL_CONNECTING	\
	|| (d)->status == GTA_DL_FALLBACK	\
	|| (d)->status == GTA_DL_PUSH_SENT	)

#define DOWNLOAD_IS_IN_PUSH_MODE(d) (d->push)

gboolean download_has_blank_guid(const struct download *);

static inline void
download_check(const struct download * const d)
{
	g_assert(d);
	g_assert(DOWNLOAD_MAGIC == d->magic);
	g_assert(!d->always_push || !download_has_blank_guid(d));
}

const char *download_pathname(const struct download *);
const char *download_basename(const struct download *);

/*
 * Public interface, visible only from the bridge.
 */

#ifdef CORE_SOURCES

/* FIXME: download_index_changed
 *        actually needs to be in downloads.h and should be called from
 *        search.h and not from search_gui.h.
 */
void download_index_changed(const host_addr_t, guint16, const struct guid *,
		guint32, guint32);

gboolean download_new(const char *filename,
	const char *uri,
	filesize_t size,
	const host_addr_t addr,
	guint16 port,
	const struct guid *guid,
	const char *hostname,
	const struct sha1 *sha1,
	const struct tth *tth,
	time_t stamp,
    struct dl_file_info *fi,
	const gnet_host_vec_t *proxies,
	guint32 flags,
	const char *parq_id);

void download_auto_new(const char *filename,
 	filesize_t size,
	const host_addr_t addr,
	guint16 port,
	const struct guid *guid,
	const char *hostname,
	const struct sha1 *sha1,
	const struct tth *tth,
	time_t stamp,
	struct dl_file_info *fi,
	gnet_host_vec_t *proxies,
	guint32 flags);

void download_dht_auto_new(const char *filename,
 	filesize_t size,
	const char *hostname,
	const host_addr_t addr,
	guint16 port,
	const struct guid *guid,
	const struct sha1 *sha1,
	const struct tth *tth,
	time_t stamp,
	struct dl_file_info *fi,
	guint32 flags);

guint download_handle_magnet(const char *url);
char *download_build_url(const struct download *);
int download_get_http_req_percent(const struct download *);
void download_fallback_to_push(struct download *, gboolean, gboolean);
int download_remove_all_from_peer(const struct guid *,
		host_addr_t addr, guint16 port, gboolean unavailable);
void download_remove_file(struct download *, gboolean reset);
gboolean download_file_exists(const struct download *);

void download_request_requeue(struct download *);
void download_request_start(struct download *);
void download_request_pause(struct download *);
gboolean download_request_remove(struct download *);
void download_request_abort(struct download *);
void download_request_resume(struct download *);

void download_freeze_queue(void);
void download_thaw_queue(void);
gboolean download_queue_is_frozen(void);

void download_gui_updates_freeze(void);
void download_gui_updates_thaw(void);

void download_clear_stopped(gboolean, gboolean, gboolean, gboolean, gboolean);
const char *download_get_hostname(const struct download *);
double download_source_progress(const struct download *);
double download_total_progress(const struct download *);
gboolean download_something_to_clear(void);
guint download_speed_avg(struct download *);

#endif /* CORE_SOURCES */
#endif /* _if_core_downloads_h_ */

/* vi: set ts=4 sw=4 cindent: */
