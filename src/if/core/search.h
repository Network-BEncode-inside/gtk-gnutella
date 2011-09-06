/*
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

#ifndef _if_core_search_h_
#define _if_core_search_h_

#include "common.h"

#include "lib/misc.h"
#include "lib/hashlist.h"
#include "lib/vendors.h"
#include "if/core/nodes.h"

/***
 *** Searches
 ***/
typedef guint32 gnet_search_t;

/*
 * Flags for search_new()
 */
enum {
	SEARCH_F_WHATS_NEW 	= 1 << 5,	/**< Start a "What's New?" search */
	SEARCH_F_LOCAL		= 1 << 4,	/**< Search in local files */
	SEARCH_F_LITERAL	= 1 << 3,	/**< Don't parse the query string */
	SEARCH_F_BROWSE		= 1 << 2,	/**< Start a browse-host search */
	SEARCH_F_ENABLED 	= 1 << 1,	/**< Start an enabled search */
	SEARCH_F_PASSIVE 	= 1 << 0	/**< Start a passive ssearch */
};

/*
 * Result sets `status' flags.
 */
enum {
	 ST_MEDIA				= (1 << 27), /**< No proper media type in hit */
	 ST_ALIEN				= (1 << 26), /**< Alien IP address in UDP hit */
	 ST_GUESS				= (1 << 25), /**< Results from a GUESS query */
	 ST_MORPHEUS_BOGUS		= (1 << 24), /**< Bogus result from Morpheus */
	 ST_GOOD_TOKEN			= (1 << 23), /**< OOB v3 matched */
	 ST_BROWSE				= (1 << 22), /**< Browse Host "search" result */
	 ST_LOCAL				= (1 << 21), /**< Local search result */
	 ST_FW2FW				= (1 << 20), /**< Firewall-to-Firewall support */
	 ST_HOSTILE				= (1 << 19), /**< From an hostile host */
	 ST_UNREQUESTED			= (1 << 18), /**< Unrequested (OOB) result */
	 ST_EVIL				= (1 << 17), /**< Carries evil filename */
	 ST_ALT_SPAM			= (1 << 16), /**< Carries alt-loc spam [UNUSED] */
	 ST_DUP_SPAM			= (1 << 15), /**< Carries spam known by URN */
	 ST_FAKE_SPAM			= (1 << 14), /**< Fake file */
	 ST_NAME_SPAM			= (1 << 13), /**< Carries alt-loc spam */
	 ST_URL_SPAM			= (1 << 12), /**< Carries action URL spam */
	 ST_URN_SPAM			= (1 << 11), /**< Carries spam known by URN */
	 ST_TLS					= (1 << 10), /**< Indicated support for TLS */
	 ST_BH					= (1 << 9),	 /**< Browse Host support */
	 ST_KNOWN_VENDOR		= (1 << 8),	 /**< Found known vendor code */
	 ST_PARSED_TRAILER		= (1 << 7),	 /**< Was able to parse trailer */
	 ST_UDP					= (1 << 6),	 /**< Got hit via UDP */
	 ST_BOGUS				= (1 << 5),	 /**< Bogus IP address */
	 ST_PUSH_PROXY			= (1 << 4),	 /**< Listed some push proxies */
	 ST_GGEP				= (1 << 3),	 /**< Trailer has a GGEP extension */
	 ST_UPLOADED			= (1 << 2),	 /**< Is "stable", people downloaded */
	 ST_BUSY				= (1 << 1),	 /**< Has currently no slots */
	 ST_FIREWALL			= (1 << 0),	 /**< Is behind a firewall */

	 ST_SPAM	= (	ST_ALT_SPAM
			 		|ST_DUP_SPAM
					|ST_FAKE_SPAM
					|ST_NAME_SPAM
					|ST_URL_SPAM
					|ST_URN_SPAM)
};

/*
 * Processing of ignored files.
 */
enum {
	SEARCH_IGN_DISPLAY_AS_IS,	/**< Display normally */
	SEARCH_IGN_DISPLAY_MARKED,	/**< Display marked (lighter color) */
	SEARCH_IGN_NO_DISPLAY		/**< Don't display */
};

/**
 * A results_set structure factorizes the common information from a Query Hit
 * packet, and then has a list of individual records, one for each hit.
 *
 * A single structure is created for each Query Hit packet we receive, but
 * then it can be dispatched for displaying some of its records to the
 * various searches in presence.
 */
typedef struct gnet_results_set {
	host_addr_t addr;
	host_addr_t last_hop;		/**< IP of delivering node */

	const struct guid *guid;	/**< Servent's GUID (atom) */
	const char *hostname;		/**< Optional: server's hostname */
	const char *version;		/**< Version information (atom) */
	const char *query;			/**< Optional: Original query string (atom) */
	gnet_host_vec_t *proxies;	/**< Optional: known push proxies */
	GSList *records;

	time_t  stamp;				/**< Reception time of the hit */
	vendor_code_t vcode;		/**< Vendor code */
	guint32 speed;
	guint32 num_recs;
	guint32 status;				/**< Parsed status bits from trailer */

    guint32 flags;
	guint16 port;
	guint16 country;			/**< Country code -- encoded ISO3166 */
	guint8 hops;
	guint8 ttl;
	guint8 media;				/**< Optional: media type filtering */
} gnet_results_set_t;

/*
 * Result record flags
 */
enum {
	SR_MEDIA		= (1 << 10),	/* Media type filter mismatch */
	SR_PARTIAL_HIT	= (1 << 9),		/* Got a hit for a partial file */
	SR_PUSH			= (1 << 8),		/* Servent firewalled, will need a PUSH */
	SR_ATOMIZED		= (1 << 7),		/* Set if filename is an atom */
	SR_PARTIAL		= (1 << 6),		/* File is being downloaded, incomplete */
	SR_OWNED		= (1 << 5),
	SR_SHARED		= (1 << 4),
	SR_SPAM			= (1 << 3),
	SR_DONT_SHOW	= (1 << 2),
	SR_IGNORED		= (1 << 1),
	SR_DOWNLOADED	= (1 << 0)
};

/**
 * An individual hit.  It referes to a file entry on the remote servent,
 * as identified by the parent results_set structure that contains this hit.
 */
typedef struct gnet_record {
	const char  *filename;		/**< File name (see SR_ATOMIZED!) */
	const struct sha1 *sha1;	/**< SHA1 URN (binary form, atom) */
	const struct tth *tth;		/**< TTH URN (binary form, atom) */
	const char  *tag;			/**< Optional tag data string (atom) */
	const char  *xml;			/**< Optional XML data string (atom) */
	const char  *path;			/**< Optional path (atom) */
	gnet_host_vec_t *alt_locs;	/**< Optional: known alternate locations */
	filesize_t size;			/**< Size of file, in bytes */
	time_t  create_time;		/**< Create Time of file; zero if unknown */
	guint32 file_index;			/**< Index for GET command */
    guint32 flags;
} gnet_record_t;

/**
 * Search query types.
 */
typedef enum {
    QUERY_STRING,
    QUERY_SHA1
} query_type_t;

/**
 * Search callbacks
 */

typedef void (*search_request_listener_t) (
    query_type_t, const char *query, const host_addr_t addr, guint16);

typedef void (*search_got_results_listener_t)
    (GSList *, const struct guid *, const gnet_results_set_t *);

typedef void (*search_status_change_listener_t)(gnet_search_t);

enum search_new_result {
	SEARCH_NEW_SUCCESS,
	SEARCH_NEW_TOO_LONG,
	SEARCH_NEW_TOO_SHORT,
	SEARCH_NEW_TOO_EARLY,
	SEARCH_NEW_INVALID_URN
};

/**
 * Media type flags that can be specified in the GGEP "M" key of queries.
 */
#define SEARCH_AUDIO_TYPE	0x0004
#define SEARCH_VIDEO_TYPE	0x0008
#define SEARCH_DOC_TYPE		0x0010
#define SEARCH_IMG_TYPE		0x0020
#define SEARCH_WIN_TYPE		0x0040
#define SEARCH_UNIX_TYPE	0x0080
#define SEARCH_TORRENT_TYPE	0x0100	/* Broken as deployed on 2011-05-15 */

/*
 * Search public interface, visible only from the bridge.
 */

#ifdef CORE_SOURCES

enum search_new_result search_new(gnet_search_t *ptr, const char *, unsigned,
			time_t create_time, guint lifetime, guint32 timeout, guint32 flags);
void search_close(gnet_search_t);

void search_start(gnet_search_t);
void search_stop(gnet_search_t);

/*  search_is_stopped doesn't exist yet!
gboolean search_is_stopped(gnet_search_t sh);
*/

void search_add_kept(gnet_search_t, const struct guid *, guint32 kept);

const char *search_query(gnet_search_t);

gboolean search_is_active(gnet_search_t);
gboolean search_is_browse(gnet_search_t);
gboolean search_is_expired(gnet_search_t);
gboolean search_is_frozen(gnet_search_t);
gboolean search_is_local(gnet_search_t);
gboolean search_is_passive(gnet_search_t);
gboolean search_is_whats_new(gnet_search_t sh);

void search_set_reissue_timeout(gnet_search_t, guint32 timeout);
guint32 search_get_reissue_timeout(gnet_search_t);
guint search_get_lifetime(gnet_search_t);
time_t search_get_create_time(gnet_search_t);
void search_set_create_time(gnet_search_t, time_t);
unsigned search_get_media_type(gnet_search_t);

void search_free_alt_locs(gnet_record_t *);

void search_update_items(gnet_search_t, guint32 items);

gboolean search_browse(gnet_search_t,
	const char *hostname, host_addr_t addr, guint16 port,
	const struct guid *guid, const gnet_host_vec_t *proxies, guint32 flags);
gboolean search_locally(gnet_search_t sh, const char *query);
guint search_handle_magnet(const char *url);

void search_got_results_listener_add(search_got_results_listener_t);
void search_got_results_listener_remove(search_got_results_listener_t);

void search_status_change_listener_add(search_status_change_listener_t);
void search_status_change_listener_remove(search_status_change_listener_t);

void search_request_listener_add(search_request_listener_t);
void search_request_listener_remove(search_request_listener_t);

void search_associate_sha1(gnet_search_t sh, const struct sha1 *sha1);
void search_dissociate_sha1(const struct sha1 *sha1);
GSList *search_associated_sha1(gnet_search_t sh);
unsigned search_associated_sha1_count(gnet_search_t sh);

const char *search_media_mask_to_string(unsigned mask);

#endif /* CORE_SOURCES */
#endif /* _if_core_search_h_ */

/* vi: set ts=4 sw=4 cindent: */
