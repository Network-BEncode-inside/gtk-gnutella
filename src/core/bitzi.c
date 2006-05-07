/*
 * $Id$
 *
 * Copyright (c) 2004, Alex Bennee <alex@bennee.com>
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
 * Bitzi Core search code.
 *
 * This code makes searches to the Bitzi (bitzi.com) meta-data
 * service. It is independent from any GUI functions and part of the
 * core of GTKG.
 *
 * @note
 * The code requires libxml to parse the XML responses.
 *
 * @author Alex Bennee <alex@bennee.com>
 * @date 2004
 */

#include "common.h"

#include <libxml/parser.h>
#include <libxml/tree.h>

#include "http.h"			/* http async stuff */
#include "bitzi.h"			/* bitzi metadata */
#include "settings.h"		/* settings_config_dir() */

#include "if/bridge/c2ui.h"
#include "if/gnet_property_priv.h"

#include "lib/atoms.h"
#include "lib/getdate.h"	/* date2time() */
#include "lib/glib-missing.h"
#include "lib/tm.h"
#include "lib/walloc.h"
#include "lib/override.h"	/* This file MUST be the last one included */

/**
 * @struct bitzi_request_t
 *
 * The bitzi_request_t structure ties together each Bitzi request
 * which are stored in the request queue.
 *
 */

static const gchar bitzi_url_fmt[] = "http://ticket.bitzi.com/rdf/urn:sha1:%s";

typedef struct {
	gchar *urnsha1;			/**< urnsha1, atom */
	gchar bitzi_url[SHA1_BASE32_SIZE + sizeof bitzi_url_fmt]; /**< request URL */

	/*
	 * xml related bits
	 */
	xmlParserCtxt *ctxt;   	/**< libxml parser context */
} bitzi_request_t;

/*
 * The request queue, the searches to the Bitzi data service are queued
 */
static GSList *bitzi_rq = NULL;

static bitzi_request_t	*current_bitzi_request = NULL;
static gpointer	 current_bitzi_request_handle;


/*
 * Hash Table/Cache for all queries we've ever done
 *
 * This allows non-blocking threads to check if we have any results
 * for the given urn:sha1. The entries are both indexed in the hash
 * table (for quick lookups) and a GList so we can go through the data
 * for expiring entries
 */

static GHashTable *bitzi_cache_ht;
static GList *bitzi_cache;

static FILE *bitzi_cache_file;

/*
 * Function declarations
 */

/* bitzi request handling */
static gboolean do_metadata_query(bitzi_request_t * req);
static void process_meta_data(bitzi_request_t * req);

/* cache functions */
static gboolean bitzi_cache_add(bitzi_data_t * data);
static void bitzi_cache_remove(bitzi_data_t * data);
static void bitzi_cache_clean(void);

/* Get rid of the obnoxious (xmlChar *) */
static inline gchar *
xml_get_string(xmlNode *node, const gchar *id)
{
	return (gchar *) xmlGetProp(node, (const xmlChar *) id);
}

static inline const xmlChar *
string_to_xmlChar(const gchar *s)
{
	/* If we were pedantic, we'd verify that ``s'' is UTF-8 encoded */
	return (const xmlChar *) s;
}

static inline const gchar *
xmlChar_to_gchar(const xmlChar *s)
{
	return (const gchar *) s;
}

/********************************************************************
 ** Bitzi Create and Destroy data structure
 ********************************************************************/

static bitzi_data_t *
bitzi_create(void)
{
	bitzi_data_t *data = walloc(sizeof(bitzi_data_t));

	/*
	 * defaults
	 */
	data->urnsha1 = NULL;
	data->mime_type = NULL;
	data->mime_desc = NULL;
	data->size = 0;
	data->goodness = 0;
	data->judgement = BITZI_FJ_UNKNOWN;
	data->expiry = (time_t) -1;

	return data;
}

static void
bitzi_destroy(bitzi_data_t *data)
{
	if (bitzi_debug)
		g_message("bitzi_destory: %p", cast_to_gconstpointer(data));

	if (data->urnsha1)
		atom_sha1_free(data->urnsha1);

	if (data->mime_type)
		G_FREE_NULL(data->mime_type);

	if (data->mime_desc)
		G_FREE_NULL(data->mime_desc);

	if (bitzi_debug)
		g_message("bitzi_destory: freeing data");
	wfree(data, sizeof *data);
}


/*********************************************************************
 ** Bitzi Query and result Parsing
 ********************************************************************/

/**
 * Populate callback: more data available. When called with 0 it stops
 * the parsing of the document tree and processes the ticket.
 */
static void
bitzi_host_data_ind(gpointer unused_handle, gchar *data, gint len)
{
	gint result;

	(void) unused_handle;

	if (len > 0) {
		result = xmlParseChunk(current_bitzi_request->ctxt, data, len, 0);

		if (result != 0)
			g_warning("bitzi_host_data_ind, bad xml result %d", result);
	} else {

		result = xmlParseChunk(current_bitzi_request->ctxt, data, 0, 1);

		if (result != 0)
			g_warning("bitzi_host_data_ind - end, bad xml result %d", result);

		/*
		 * process what we had and clear up
		 */
		process_meta_data(current_bitzi_request);

		current_bitzi_request = NULL;
		current_bitzi_request_handle = NULL;
	}
}

/**
 * HTTP request is being stopped.
 */
static void
bitzi_host_error_ind(gpointer handle,
	http_errtype_t unused_type, gpointer unused_v)
{
	(void) unused_type;
	(void) unused_v;

	g_warning("bitzi_host_error_ind: failed!");

	g_assert(handle == current_bitzi_request_handle);

	/*
	 * process what we had and clear up
	 */
	process_meta_data(current_bitzi_request);

	current_bitzi_request = NULL;
	current_bitzi_request_handle = NULL;
}

/*
 * These XML parsing routines are hacked up versions of those from the
 * libxml2 examples.
 */


/**
 * Parse (and eventually fill in) the bitzi specific data.
 *
 * The fields are defined at:
 *	schema: http://bitzi.com/developer/bitzi-ticket.rng
 *	notes: http://bitzi.com/openbits/datadump
 *
 * The ones we have most interest in are:
 *
 * 	bz:fileGoodness="2.1"
 * 	bz:fileJudgement="Complete"
 *
 * Although the other could be used to verify size data and such.
 */

struct efj_t {
	const gchar *string;
	bitzi_fj_t judgement;
};

static const struct efj_t enum_fj_table[] = {
	{ "Unknown",				BITZI_FJ_UNKNOWN },
	{ "Dangerous/Misleading",	BITZI_FJ_DANGEROUS_MISLEADING },
	{ "Incomplete/Damaged",		BITZI_FJ_INCOMPLETE_DAMAGED },
	{ "Substandard",			BITZI_FJ_SUBSTANDARD },
	{ "Overrated",				BITZI_FJ_OVERRATED },
	{ "Normal",					BITZI_FJ_NORMAL },
	{ "Underrated",				BITZI_FJ_UNDERRATED },
	{ "Complete",				BITZI_FJ_COMPLETE },
	{ "Recommended",			BITZI_FJ_RECOMMENDED },
	{ "Best Version",			BITZI_FJ_BEST_VERSION }
};

/**
 * Read all the attributes we may want from the rdf ticket, some
 * atributes will not be there in which case xmlGetProp will return a null.
 */
static void
process_rdf_description(xmlNode *node, bitzi_data_t *data)
{
	gchar *s;

	/*
	 * We extract the urn:sha1 from the ticket as we may be processing
	 * cached tickets not associated with any actual request. The
	 * bitprint itself will be at offset 9 into the string.
	 */
	s = xml_get_string(node, "about");
	if (s) {
		static const gchar urn_prefix[] = "urn:sha1:";
		const gchar *sha1;

		sha1 = is_strprefix(s, urn_prefix);
		if (sha1) {
			size_t len;

			/* Skip the "urn:sha1:" prefix and check whether an SHA1
			 * follows. We need to ensure that buf contains at least
			 * SHA1_BASE32_SIZE bytes because that's what base32_sha1()
			 * assumes. We allow trailing characters. */

			len = strlen(sha1);
			sha1 = len < SHA1_BASE32_SIZE ? NULL : base32_sha1(sha1);
		}

		if (!sha1) {
			g_warning("process_rdf_description: bad 'about' string: \"%s\"", s);
		} else {
			data->urnsha1 = atom_sha1_get(sha1);
		}
	} else {
		g_warning("process_rdf_description: No urnsha!");
	}


	/*
	 * All tickets have a ticketExpires tag which we need for cache
	 * managment.
	 *
	 * CHECK: date parse deals with timezone? can it fail?
	 */
	s = xml_get_string(node, "ticketExpires");
	if (s) {
		data->expiry = date2time(s, tm_time());
		if ((time_t) -1 == data->expiry)
			g_warning("process_rdf_description: Bad expiration date \"%s\"", s);
	} else {
		g_warning("process_rdf_description: No ticketExpires!");
	}

	/*
	 * fileGoodness amd fileJudgement are the two most imeadiatly
	 * useful values.
	 */
	s = xml_get_string(node, "fileGoodness");
	if (s) {
		data->goodness = g_strtod(s, NULL);
		if (bitzi_debug)
			g_message("fileGoodness is %s/%f", s, data->goodness);
	} else {
		data->goodness = 0;
	}

	data->judgement = BITZI_FJ_UNKNOWN;

	s = xml_get_string(node, "fileJudgement");
	if (s) {
		size_t i;

		STATIC_ASSERT(NUM_BITZI_FJ == G_N_ELEMENTS(enum_fj_table));

		for (i = 0; i < G_N_ELEMENTS(enum_fj_table); i++) {
			if (
				xmlStrEqual(string_to_xmlChar(s),
					string_to_xmlChar(enum_fj_table[i].string))
			) {
				data->judgement = enum_fj_table[i].judgement;
				break;
			}
		}
	}


	/*
	 * fileLength, useful for comparing to result
	 */

	s = xml_get_string(node, "fileLength");
	if (s) {
		gint error;
		data->size = parse_uint64(s, NULL, 10, &error);
	}

	/*
	 * The multimedia type, bitrate etc is all built into one
	 * descriptive string. It is dependant on format
	 *
	 * Currently we handle video and audio
	 */

	s = xml_get_string(node, "format");
	if (s) {
		if (xmlStrstr(string_to_xmlChar(s), string_to_xmlChar("video"))) {
			gchar *xml_width = xml_get_string(node, "videoWidth");
			gchar *xml_height = xml_get_string(node, "videoHeight");
			gchar *xml_bitrate = xml_get_string(node, "videoBitrate");
			gchar *xml_fps = xml_get_string(node, "videoFPS");

			/*
			 * copy the mime type
			 */
			data->mime_type = g_strdup(s);

			/*
			 * format the mime details
			 */
			{
				gboolean has_res = xml_width && xml_height;
				/**
	 			 * TRANSLATORS: This describes video parameters;
				 * The first part is used as <width>x<height> (resolution).
				 * fps stands for "frames per second".
				 * kbps stands for "kilobit per second" (metric kilo).
	 			 */
				data->mime_desc =
					g_strdup_printf(_("%s%s%s%s%s fps, %s kbps"),
						has_res ? xml_width : "",
						has_res ? Q_("times|x") : "",
						has_res ? xml_height : "",
						has_res ? ", " : "",
						(xml_fps != NULL) ? xml_fps : "?",
						(xml_bitrate != NULL) ? xml_bitrate : "?");
			}
		} else if (
			xmlStrstr(string_to_xmlChar(s), string_to_xmlChar("audio"))
		) {
			data->mime_type = g_strdup(s);
		}
	}

	/*
	 ** For debugging/development - dump all the attributes
	 */

	if (bitzi_debug) {
		xmlAttr *cur_attr;

		for (cur_attr = node->properties; cur_attr; cur_attr = cur_attr->next) {
			const gchar *name = xmlChar_to_gchar(cur_attr->name);

			g_message("bitzi rdf attrib: %s, type %d = %s",
				name, cur_attr->type, xml_get_string(node, name));
		}
	}
}

/**
 * Iterates through the XML/RDF ticket calling various process
 * functions to read the data into the bitzi_data_t.
 *
 * This function is recursive, if the element is not explicity know we
 * just recurse down a level.
 */
static void
process_bitzi_ticket(xmlNode *a_node, bitzi_data_t *data)
{
	xmlNode *cur_node = NULL;

	for (cur_node = a_node; cur_node; cur_node = cur_node->next) {
		if (cur_node->type == XML_ELEMENT_NODE) {
			if (bitzi_debug)
				g_message("node type: Element, name: %s, children %p",
					cur_node->name, cast_to_gconstpointer(cur_node->children));

			if (
				0 == xmlStrcmp(cur_node->name,
					string_to_xmlChar("Description"))
			)
				process_rdf_description(cur_node, data);
			else
				process_bitzi_ticket(cur_node->children, data);
		}
	}
}

/**
 * Walk the parsed document tree and free up the data.
 */
static void
process_meta_data(bitzi_request_t *request)
{
	xmlDoc	*doc; 	/* the resulting document tree */
	xmlNode	*root;
	gint result;

	if (bitzi_debug)
		g_message("process_meta_data: %p", cast_to_gconstpointer(request));

	g_assert(request != NULL);

	/*
	 * Get the document and free context
	 */

	doc = request->ctxt->myDoc;
	result = request->ctxt->wellFormed;
	xmlFreeParserCtxt(request->ctxt);

	if (bitzi_debug)
		g_message("process_meta_data: doc = %p, result = %d",
			cast_to_gconstpointer(doc), result);

	/*
	 * Now we can have a look at the data
	 */

	if (result) {
		bitzi_data_t *data;

	   	data = bitzi_create();

		/*
		 * This just dumps the data
		 */

		root = xmlDocGetRootElement(doc);
		process_bitzi_ticket(root, data);
		if (!data->urnsha1) {
			g_warning("process_meta_data: missing urn:sha1");
		} else {

			/*
			 * If the data has a valid date then we can cache the result
			 * and re-echo the XML ticket to the file based cache.
			 */

			if (
				(time_t) -1 == data->expiry ||
				delta_time(data->expiry, tm_time()) <= 0
			) {
				g_warning("process_meta_data: stale bitzi data");
			} else if (bitzi_cache_add(data)) {
				if (bitzi_cache_file) {
					xmlDocDump(bitzi_cache_file, doc);
					fputs("\n", bitzi_cache_file);
				}

				gcu_bitzi_result(data);
			}
		}

		/* we are now finished with this XML doc */
		xmlFreeDoc(doc);
	}

	/*
	 * free used memory by the request
	 */

	if (request->urnsha1)
		atom_sha1_free(request->urnsha1);

	wfree(request, sizeof *request);
}

/**
 * Send a meta-data query
 *
 * Called directly when a request launched or via the bitzi_heartbeat tick.
 */
static gboolean
do_metadata_query(bitzi_request_t *req)
{
	if (bitzi_debug)
		g_message("do_metadata_query: %p", cast_to_gconstpointer(req));

	/*
	 * always remove the request from the queue
	 */
	bitzi_rq = g_slist_remove(bitzi_rq, req);

	/*
	 * check we haven't already got a response from a previous query
	 */
	if (NULL != bitzi_querycache_byurnsha1(req->urnsha1))
		return FALSE;

	current_bitzi_request = req;

	/*
	 * Create the XML Parser
	 */

	current_bitzi_request->ctxt = xmlCreatePushParserCtxt(
		NULL, NULL, NULL, 0, current_bitzi_request->bitzi_url);

	g_assert(current_bitzi_request->ctxt != NULL);

	/*
	 * Launch the asynchronous request and attach parsing
	 * information.
	 *
	 * We don't care about headers
	 */

	current_bitzi_request_handle =
		http_async_get(current_bitzi_request->bitzi_url, NULL,
			bitzi_host_data_ind, bitzi_host_error_ind);

		if (!current_bitzi_request_handle) {
			g_warning("could not launch a \"GET %s\" request: %s",
					current_bitzi_request->bitzi_url,
					http_async_strerror(http_async_errno));
		} else {
			if (bitzi_debug)
				g_message("do_metadata_query: request %s launched",
					current_bitzi_request->bitzi_url);
			return TRUE;
		}

	/*
	 * no query launched
	 */

	return FALSE;
}

/**************************************************************
 ** Bitzi Results Cache
 *************************************************************/

/**
 * Add the data entry to the cache and in expiry sorted date order to
 * the linked list.
 */
static gint
bitzi_date_compare(gconstpointer p, gconstpointer q)
{
	const bitzi_data_t *a = p, *b = q;

	return delta_time(a->expiry, b->expiry);
}

static gboolean
bitzi_cache_add(bitzi_data_t *data)
{
	g_assert(data);
	g_assert(data->urnsha1);

	if (g_hash_table_lookup(bitzi_cache_ht, data->urnsha1) != NULL) {
		g_warning("bitzi_cache_add: duplicate entry!");
		return FALSE;
	}

	g_hash_table_insert(bitzi_cache_ht, data->urnsha1, data);
	bitzi_cache = g_list_insert_sorted(bitzi_cache, data, bitzi_date_compare);

	if (bitzi_debug)
		g_message("bitzi_cache_add: data %p, now %u entries",
			cast_to_gconstpointer(data), g_hash_table_size(bitzi_cache_ht));

	return TRUE;
}

static void
bitzi_cache_remove(bitzi_data_t * data)
{
	if (bitzi_debug)
		g_message("bitzi_cache_remove: %p", cast_to_gconstpointer(data));

	g_assert(data);
	g_assert(data->urnsha1);

	g_hash_table_remove(bitzi_cache_ht, data->urnsha1);
	bitzi_cache = g_list_remove(bitzi_cache, data);

	/*
	 * destroy when done
	 */
	bitzi_destroy(data);
}

static void
bitzi_cache_clean(void)
{
	time_t now = tm_time();
	GList *l = bitzi_cache;
	GSList *to_remove = NULL, *sl;

	/*
	 * find all entries that have expired
	 */

	for (l = bitzi_cache; l != NULL; l = g_list_next(l)) {
		bitzi_data_t *data = l->data;

		if (delta_time(data->expiry, now) >= 0)
			break;

		to_remove = g_slist_prepend(to_remove, data);
	}

	/*
	 * now flush the expired entries
	 */

	for (sl = to_remove; sl != NULL; sl = g_slist_next(sl))
		bitzi_cache_remove(sl->data);

	g_slist_free(to_remove);
}

/*************************************************************
 ** Bitzi Heartbeat
 ************************************************************/

/**
 * The heartbeat function is a repeating glib timeout that is used to
 * pace queries to the bitzi metadata service. It also periodically
 * runs the bitzi_cache_clean routine to clean the cache.
 */
static gboolean
bitzi_heartbeat(gpointer unused_data)
{
	(void) unused_data;

	/*
	 * launch any pending queries
	 */

	while (current_bitzi_request == NULL && bitzi_rq != NULL) {
		if (do_metadata_query(bitzi_rq->data))
			break;
	}

	bitzi_cache_clean();

	return TRUE;		/* Always requeue */
}


/**************************************************************
 ** Bitzi API
 *************************************************************/

/**
 * Query the bitzi cache for this given urnsha1, return NULL if
 * nothing otherwise we return the
 */
bitzi_data_t *
bitzi_querycache_byurnsha1(const gchar *urnsha1)
{
	g_return_val_if_fail(NULL != urnsha1, NULL);
	return g_hash_table_lookup(bitzi_cache_ht, urnsha1);
}

/**
 * A GUI/Bitzi API passes a pointer to the search type (currently only
 * urn:sha1), a pointer to a callback function and a user data
 * pointer.
 *
 * If no query succeds then the call back is never made, however we
 * should always get some sort of data back from the service.
 */
gpointer
bitzi_query_byurnsha1(const gchar *urnsha1)
{
	bitzi_data_t *data = NULL;
	bitzi_request_t	*request;

	g_return_val_if_fail(NULL != urnsha1, NULL);

	data = bitzi_querycache_byurnsha1(urnsha1);
	if (data == NULL) {
		size_t len;

		request = walloc(sizeof *request);

		/*
		 * build the bitzi url
		 */
		request->urnsha1 = atom_sha1_get(urnsha1);
		len = gm_snprintf(request->bitzi_url, sizeof request->bitzi_url,
				bitzi_url_fmt, sha1_base32(urnsha1));
		g_assert(len < sizeof request->bitzi_url);

		bitzi_rq = g_slist_append(bitzi_rq, request);
		if (bitzi_debug) {
			g_message("bitzi_queryby_urnsha1: queued query, %d in queue",
				g_slist_position(bitzi_rq, g_slist_last(bitzi_rq)) + 1);
		}

		/*
		 * the heartbeat will pick up the request
		 */
	} else {
		if (bitzi_debug)
			g_message("bitzi_queryby_urnsha1: result already in cache");
				gcu_bitzi_result(data);
	}

	return data;
}

/**
 * Initialise any bitzi specific stuff we want to here.
 */
void
bitzi_init(void)
{
	FILE *old_data;
	gchar *path, *oldpath;
	gint ticket_count = 0;

	bitzi_cache_ht = g_hash_table_new(NULL, NULL);

	/*
	 * Rename the old file , overwritting stuff if we have to
	 */
	oldpath = make_pathname(settings_config_dir(), "bitzi.xml.orig");
  	path = make_pathname(settings_config_dir(), "bitzi.xml");

	g_assert(NULL != path);
	g_assert(NULL != oldpath);

	if (rename(path, oldpath)) {
		g_warning("bitzi_init: failed to rename %s to %s (%s)",
				path, oldpath, g_strerror(errno));
	}

	/*
	 * Set up the file cache descriptor, starting from scratch.
	 */
	bitzi_cache_file = fopen(path, "w");
	if (!bitzi_cache_file) {
		g_warning("bitzi_init: failed to open bitzi cache (%s) %s",
				path, g_strerror(errno));
	}

	/*
	 * "play" the .orig file back through the XML parser and
	 * repopulate our internal cache
	 */
	old_data = fopen(oldpath, "r");
	if (!old_data) {
		if (errno != ENOENT)
			g_warning("Failed to open %s for cached Bitzi data (%s)",
				oldpath, g_strerror(errno));
	} else {
		bitzi_request_t *request = NULL;
		gboolean truncated = FALSE;
		gchar tmp[1024];

		for (;;) {
			static const char xml_prefix[] = "<?xml";
			gboolean eof, eot;

			eof = NULL == fgets(tmp, sizeof(tmp), old_data);
			eot = !eof && !truncated && is_strprefix(tmp, xml_prefix);

			/*
			 * Each XML ticket will start with an XML header at which
			 * time we submit the last piece of data and set up a new
			 * context for the next ticket.
			 */

			if ((eof || eot) && request) {
				int result;

				/* finish parsing */
				result = xmlParseChunk(request->ctxt, tmp, 0, 1);
				if (0 == result)
					ticket_count++;
				else
					g_warning("bitzi_init: "
						"bad xml parsing cache result %d", result);

				process_meta_data(request);
			}

			if (eof)
				break;

			if (eot) {
				/* new pseudo request */
				request = walloc(sizeof *request);
				request->urnsha1 = NULL;

				request->ctxt = xmlCreatePushParserCtxt(
					NULL, NULL, NULL, 0, NULL);
			}

			truncated = NULL == strchr(tmp, '\n');

			/*
			 * While we have a request stream the data into the XML
			 * parser, much like bitzi_host_data_ind does
			 */
			if (request) {
		   		size_t len;

				len = strlen(tmp);
				if (len > 0) {
					int result;

					result = xmlParseChunk(request->ctxt, tmp, len, 0);
					if (result != 0)
						g_warning("bitzi_init: "
							"bad xml parsing cache result %d", result);

				}
			}

		} /* for (;;) */

		fclose(old_data);
	} /* if (old_data) */

	if (bitzi_debug)
		g_message("Loaded %d bitzi ticket(s) from \"%s\"",
			ticket_count, oldpath);

	/* clean-up */
	G_FREE_NULL(path);
	G_FREE_NULL(oldpath);

	/*
	 * Finally start the bitzi heart beat that will send requests when
	 * we set them up.
	 */

	g_timeout_add(1 * 10000, bitzi_heartbeat, NULL);
}

/* vi: set ts=4 sw=4 cindent: */
/* -*- mode: cc-mode; tab-width:4; -*- */
