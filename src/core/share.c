/*
 * $Id$
 *
 * Copyright (c) 2001-2005, Raphael Manfredi
 * Copyright (c) 2000 Daniel Walker (dwalker@cats.ucsc.edu)
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
 * Handle sharing of our own files and answers to remote queries.
 *
 * @author Daniel Walker (dwalker@cats.ucsc.edu)
 * @date 2000
 * @author Raphael Manfredi
 * @date 2001-2005
 */

#include "common.h"

RCSID("$Id$");

#include "share.h"
#include "gmsg.h"
#include "huge.h"
#include "qrp.h"
#include "extensions.h"
#include "nodes.h"
#include "uploads.h"
#include "gnet_stats.h"
#include "search.h"		/* For QUERY_SPEED_MARK */
#include "guid.h"
#include "hostiles.h"
#include "qhit.h"
#include "oob.h"
#include "oob_proxy.h"
#include "fileinfo.h"
#include "settings.h"
#include "hosts.h"

#include "if/gnet_property.h"
#include "if/gnet_property_priv.h"
#include "if/bridge/c2ui.h"

#include "lib/atoms.h"
#include "lib/endian.h"
#include "lib/file.h"
#include "lib/listener.h"
#include "lib/glib-missing.h"
#include "lib/tm.h"
#include "lib/utf8.h"
#include "lib/walloc.h"
#include "lib/override.h"		/* Must be the last header included */

static const guchar iso_8859_1[96] = {
	' ', 			/**< 160 - NO-BREAK SPACE */
	' ', 			/**< 161 - INVERTED EXCLAMATION MARK */
	' ', 			/**< 162 - CENT SIGN */
	' ', 			/**< 163 - POUND SIGN */
	' ', 			/**< 164 - CURRENCY SIGN */
	' ', 			/**< 165 - YEN SIGN */
	' ', 			/**< 166 - BROKEN BAR */
	' ', 			/**< 167 - SECTION SIGN */
	' ', 			/**< 168 - DIAERESIS */
	' ', 			/**< 169 - COPYRIGHT SIGN */
	'a', 			/**< 170 - FEMININE ORDINAL INDICATOR */
	' ', 			/**< 171 - LEFT-POINTING DOUBLE ANGLE QUOTATION MARK */
	' ', 			/**< 172 - NOT SIGN */
	' ', 			/**< 173 - SOFT HYPHEN */
	' ', 			/**< 174 - REGISTERED SIGN */
	' ', 			/**< 175 - MACRON */
	' ', 			/**< 176 - DEGREE SIGN */
	' ', 			/**< 177 - PLUS-MINUS SIGN */
	'2', 			/**< 178 - SUPERSCRIPT TWO */
	'3', 			/**< 179 - SUPERSCRIPT THREE */
	' ', 			/**< 180 - ACUTE ACCENT */
	'u', 			/**< 181 - MICRO SIGN */
	' ', 			/**< 182 - PILCROW SIGN */
	' ', 			/**< 183 - MIDDLE DOT */
	' ', 			/**< 184 - CEDILLA */
	'1', 			/**< 185 - SUPERSCRIPT ONE */
	'o', 			/**< 186 - MASCULINE ORDINAL INDICATOR */
	' ', 			/**< 187 - RIGHT-POINTING DOUBLE ANGLE QUOTATION MARK */
	' ', 			/**< 188 - VULGAR FRACTION ONE QUARTER */
	' ', 			/**< 189 - VULGAR FRACTION ONE HALF */
	' ', 			/**< 190 - VULGAR FRACTION THREE QUARTERS */
	' ', 			/**< 191 - INVERTED QUESTION MARK */
	'a', 			/**< 192 - LATIN CAPITAL LETTER A WITH GRAVE */
	'a', 			/**< 193 - LATIN CAPITAL LETTER A WITH ACUTE */
	'a', 			/**< 194 - LATIN CAPITAL LETTER A WITH CIRCUMFLEX */
	'a', 			/**< 195 - LATIN CAPITAL LETTER A WITH TILDE */
	'a', 			/**< 196 - LATIN CAPITAL LETTER A WITH DIAERESIS */
	'a', 			/**< 197 - LATIN CAPITAL LETTER A WITH RING ABOVE */
	' ', 			/**< 198 - LATIN CAPITAL LETTER AE */
	'c', 			/**< 199 - LATIN CAPITAL LETTER C WITH CEDILLA */
	'e', 			/**< 200 - LATIN CAPITAL LETTER E WITH GRAVE */
	'e', 			/**< 201 - LATIN CAPITAL LETTER E WITH ACUTE */
	'e', 			/**< 202 - LATIN CAPITAL LETTER E WITH CIRCUMFLEX */
	'e', 			/**< 203 - LATIN CAPITAL LETTER E WITH DIAERESIS */
	'i', 			/**< 204 - LATIN CAPITAL LETTER I WITH GRAVE */
	'i', 			/**< 205 - LATIN CAPITAL LETTER I WITH ACUTE */
	'i',			/**< 206 - LATIN CAPITAL LETTER I WITH CIRCUMFLEX */
	'i',			/**< 207 - LATIN CAPITAL LETTER I WITH DIAERESIS */
	' ',			/**< 208 - LATIN CAPITAL LETTER ETH */
	'n',			/**< 209 - LATIN CAPITAL LETTER N WITH TILDE */
	'o',			/**< 210 - LATIN CAPITAL LETTER O WITH GRAVE */
	'o',			/**< 211 - LATIN CAPITAL LETTER O WITH ACUTE */
	'o',			/**< 212 - LATIN CAPITAL LETTER O WITH CIRCUMFLEX */
	'o',			/**< 213 - LATIN CAPITAL LETTER O WITH TILDE */
	'o',			/**< 214 - LATIN CAPITAL LETTER O WITH DIAERESIS */
	' ',			/**< 215 - MULTIPLICATION SIGN */
	'o',			/**< 216 - LATIN CAPITAL LETTER O WITH STROKE */
	'u',			/**< 217 - LATIN CAPITAL LETTER U WITH GRAVE */
	'u',			/**< 218 - LATIN CAPITAL LETTER U WITH ACUTE */
	'u',			/**< 219 - LATIN CAPITAL LETTER U WITH CIRCUMFLEX */
	'u',			/**< 220 - LATIN CAPITAL LETTER U WITH DIAERESIS */
	'y',			/**< 221 - LATIN CAPITAL LETTER Y WITH ACUTE */
	' ',			/**< 222 - LATIN CAPITAL LETTER THORN */
	's',			/**< 223 - LATIN SMALL LETTER SHARP S */
	'a',			/**< 224 - LATIN SMALL LETTER A WITH GRAVE */
	'a',			/**< 225 - LATIN SMALL LETTER A WITH ACUTE */
	'a',			/**< 226 - LATIN SMALL LETTER A WITH CIRCUMFLEX */
	'a',			/**< 227 - LATIN SMALL LETTER A WITH TILDE */
	'a',			/**< 228 - LATIN SMALL LETTER A WITH DIAERESIS */
	'a',			/**< 229 - LATIN SMALL LETTER A WITH RING ABOVE */
	' ',			/**< 230 - LATIN SMALL LETTER AE */
	'c',			/**< 231 - LATIN SMALL LETTER C WITH CEDILLA */
	'e',			/**< 232 - LATIN SMALL LETTER E WITH GRAVE */
	'e',			/**< 233 - LATIN SMALL LETTER E WITH ACUTE */
	'e',			/**< 234 - LATIN SMALL LETTER E WITH CIRCUMFLEX */
	'e',			/**< 235 - LATIN SMALL LETTER E WITH DIAERESIS */
	'i',			/**< 236 - LATIN SMALL LETTER I WITH GRAVE */
	'i',			/**< 237 - LATIN SMALL LETTER I WITH ACUTE */
	'i',			/**< 238 - LATIN SMALL LETTER I WITH CIRCUMFLEX */
	'i',			/**< 239 - LATIN SMALL LETTER I WITH DIAERESIS */
	' ',			/**< 240 - LATIN SMALL LETTER ETH */
	'n',			/**< 241 - LATIN SMALL LETTER N WITH TILDE */
	'o',			/**< 242 - LATIN SMALL LETTER O WITH GRAVE */
	'o',			/**< 243 - LATIN SMALL LETTER O WITH ACUTE */
	'o',			/**< 244 - LATIN SMALL LETTER O WITH CIRCUMFLEX */
	'o',			/**< 245 - LATIN SMALL LETTER O WITH TILDE */
	'o',			/**< 246 - LATIN SMALL LETTER O WITH DIAERESIS */
	' ',			/**< 247 - DIVISION SIGN */
	'o',			/**< 248 - LATIN SMALL LETTER O WITH STROKE */
	'u',			/**< 249 - LATIN SMALL LETTER U WITH GRAVE */
	'u',			/**< 250 - LATIN SMALL LETTER U WITH ACUTE */
	'u',			/**< 251 - LATIN SMALL LETTER U WITH CIRCUMFLEX */
	'u',			/**< 252 - LATIN SMALL LETTER U WITH DIAERESIS */
	'y',			/**< 253 - LATIN SMALL LETTER Y WITH ACUTE */
	' ',			/**< 254 - LATIN SMALL LETTER THORN */
	'y',			/**< 255 - LATIN SMALL LETTER Y WITH DIAERESIS */
};

static const guchar cp1252[30] = {

	' ', 			/**< 130 - LOW-9 QUOTE */
	' ', 			/**< 131 - */
	' ', 			/**< 132 - LOW-9 DOUBLE QUOTE */
	' ', 			/**< 133 - ELLIPSES */
	' ', 			/**< 134 - DAGGER */
	' ', 			/**< 135 - DOUBLE DAGGER */
	' ', 			/**< 138 - */
	' ', 			/**< 137 - PER MILLE SIGN */
	's', 			/**< 138 - S WITH CARON */
	' ', 			/**< 139 - LEFT-POINTING ANGLE */
	' ', 			/**< 140 - */
	' ', 			/**< 141 - */
	' ', 			/**< 142 - */
	' ', 			/**< 143 - */
	' ', 			/**< 144 - */
	' ', 			/**< 145 - LEFT SINGLE QUOTE */
	' ', 			/**< 146 - RIGHT SINGLE QUOTE  */
	' ', 			/**< 147 - LEFT DOUBLE QUOTE */
	' ', 			/**< 148 - RIGHT DOUBLE QUOTE */
	' ', 			/**< 149 - BULLET */
	' ', 			/**< 150 - EN DASH */
	' ', 			/**< 151 - EM DASH */
	' ', 			/**< 152 - SMALL TILDE */
	't', /* tm */	/**< 153 - TRADEMARK */
	's', 			/**< 154 - s WITH CARON */
	' ', 			/**< 155 - RIGHT-POINTING ANGLE */
	' ', 			/**< 156 - */
	' ', 			/**< 157 - */
	' ', 			/**< 158 - */
	'y', 			/**< 159 - Y DIAERESIS */
};

static const guchar macroman[126] = {

	' ', 			/**< 130 - LOW-9 QUOTE */
	' ', 			/**< 131 - */
	' ', 			/**< 132 - LOW-9 DOUBLE QUOTE */
	' ', 			/**< 133 - ELLIPSES */
	' ', 			/**< 134 - DAGGER */
	' ', 			/**< 135 - DOUBLE DAGGER */
	' ', 			/**< 138 - */
	' ', 			/**< 137 - PER MILLE SIGN */
	's', 			/**< 138 - S WITH CARON */
	' ', 			/**< 139 - LEFT-POINTING ANGLE */
	' ', 			/**< 140 - */
	' ', 			/**< 141 - */
	' ', 			/**< 142 - */
	' ', 			/**< 143 - */
	' ', 			/**< 144 - */
	' ', 			/**< 145 - LEFT SINGLE QUOTE */
	' ', 			/**< 146 - RIGHT SINGLE QUOTE  */
	' ', 			/**< 147 - LEFT DOUBLE QUOTE */
	' ', 			/**< 148 - RIGHT DOUBLE QUOTE */
	' ', 			/**< 149 - BULLET */
	' ', 			/**< 150 - EN DASH */
	' ', 			/**< 151 - EM DASH */
	' ', 			/**< 152 - SMALL TILDE */
	't', /* tm */	/**< 153 - TRADEMARK */
	's', 			/**< 154 - s WITH CARON */
	' ', 			/**< 155 - RIGHT-POINTING ANGLE */
	' ', 			/**< 156 - */
	' ', 			/**< 157 - */
	' ', 			/**< 158 - */
	'y', 			/**< 159 - Y DIAERESIS */
	' ', 			/**< 160 - NO-BREAK SPACE */
	' ', 			/**< 161 - DEGREE */
	' ', 			/**< 162 - CENT SIGN */
	' ', 			/**< 163 - POUND SIGN */
	' ', 			/**< 164 - CURRENCY SIGN */
	' ', 			/**< 165 - BULLET */
	' ', 			/**< 166 - PARAGRAPH */
	' ', 			/**< 167 - SECTION SIGN */
	' ', 			/**< 168 - DIAERESIS */
	' ', 			/**< 169 - COPYRIGHT SIGN */
	't', /* tm */	/**< 170 - TRADEMARK */
	' ', 			/**< 171 - LEFT-POINTING DOUBLE ANGLE QUOTATION MARK */
	' ', 			/**< 172 - NOT SIGN */
	' ', 			/**< 173 - NOT EQUAL */
	' ', 			/**< 174 - REGISTERED SIGN */
	' ', 			/**< 175 - MACRON */
	' ', 			/**< 176 - INFINITY */
	' ', 			/**< 177 - PLUS-MINUS SIGN */
	' ', 			/**< 178 - LESSSOREQUAL */
	' ', 			/**< 179 - GREATOREQUAL */
	' ', 			/**< 180 - ACUTE ACCENT */
	'u', 			/**< 181 - MICRO SIGN */
	' ', 			/**< 182 - DERIVATIVE */
	' ', 			/**< 183 - SIGMA */
	' ', 			/**< 184 - CEDILLA */
	'1', 			/**< 185 - SUPERSCRIPT ONE */
	' ', 			/**< 186 - INTEGRAL */
	' ', 			/**< 187 - RIGHT-POINTING DOUBLE ANGLE QUOTATION MARK */
	' ', 			/**< 188 - VULGAR FRACTION ONE QUARTER */
	' ', 			/**< 189 - VULGAR FRACTION ONE HALF */
	' ', 			/**< 190 - VULGAR FRACTION THREE QUARTERS */
	' ', 			/**< 191 - INVERTED QUESTION MARK */
	'a', 			/**< 192 - LATIN CAPITAL LETTER A WITH GRAVE */
	'a', 			/**< 193 - LATIN CAPITAL LETTER A WITH ACUTE */
	'a', 			/**< 194 - LATIN CAPITAL LETTER A WITH CIRCUMFLEX */
	' ', 			/**< 195 - SQUARE ROOT */
	'a', 			/**< 196 - LATIN CAPITAL LETTER A WITH DIAERESIS */
	' ', 			/**< 197 - WAVY EQUAL */
	' ', 			/**< 198 - DELTA */
	'c', 			/**< 199 - LATIN CAPITAL LETTER C WITH CEDILLA */
	'e', 			/**< 200 - LATIN CAPITAL LETTER E WITH GRAVE */
	' ', 			/**< 201 - ELLIPSES */
	'e', 			/**< 202 - LATIN CAPITAL LETTER E WITH CIRCUMFLEX */
	'e', 			/**< 203 - LATIN CAPITAL LETTER E WITH DIAERESIS */
	'i', 			/**< 204 - LATIN CAPITAL LETTER I WITH GRAVE */
	'i', 			/**< 205 - LATIN CAPITAL LETTER I WITH ACUTE */
	'i',			/**< 206 - LATIN CAPITAL LETTER I WITH CIRCUMFLEX */
	'i',			/**< 207 - LATIN CAPITAL LETTER I WITH DIAERESIS */
	' ',			/**< 208 - EN DASH */
	' ',			/**< 209 - EM DASH */
	' ',			/**< 210 - LEFT DOUBLE QUOTE */
	' ',			/**< 211 - RIGHT DOUBLE QUOTE */
	' ',			/**< 212 - LEFT SINGLE QUOTE */
	' ',			/**< 213 - RIGHT SINGLE QUOTE */
	'o',			/**< 214 - LATIN CAPITAL LETTER O WITH DIAERESIS */
	' ',			/**< 215 - DIAMOND */
	'o',			/**< 216 - LATIN CAPITAL LETTER O WITH STROKE */
	'y',			/**< 217 - Y DIAERESIS */
	' ',			/**< 218 - DIVISION SLASH */
	'u',			/**< 219 - LATIN CAPITAL LETTER U WITH CIRCUMFLEX */
	' ',			/**< 220 - LEFT-POINTING ANGLE */
	' ',			/**< 221 - RIGHT-POINTING ANGLE */
	' ',			/**< 222 - LATIN CAPITAL LETTER THORN */
	's',			/**< 223 - LATIN SMALL LETTER SHARP S */
	'a',			/**< 224 - LATIN SMALL LETTER A WITH GRAVE */
	' ',			/**< 225 - PERIOD CENTERED */
	' ',			/**< 226 - LOW-9 QUOTE */
	' ',			/**< 227 - LOW-9 DOUBLE QUOTE */
	' ',			/**< 228 - PER MILLE SIGN */
	'a',			/**< 229 - LATIN SMALL LETTER A WITH RING ABOVE */
	' ',			/**< 230 - LATIN SMALL LETTER AE */
	'c',			/**< 231 - LATIN SMALL LETTER C WITH CEDILLA */
	'e',			/**< 232 - LATIN SMALL LETTER E WITH GRAVE */
	'e',			/**< 233 - LATIN SMALL LETTER E WITH ACUTE */
	'e',			/**< 234 - LATIN SMALL LETTER E WITH CIRCUMFLEX */
	'e',			/**< 235 - LATIN SMALL LETTER E WITH DIAERESIS */
	'i',			/**< 236 - LATIN SMALL LETTER I WITH GRAVE */
	'i',			/**< 237 - LATIN SMALL LETTER I WITH ACUTE */
	'i',			/**< 238 - LATIN SMALL LETTER I WITH CIRCUMFLEX */
	'i',			/**< 239 - LATIN SMALL LETTER I WITH DIAERESIS */
	' ',			/**< 240 - APPLE LOGO */
	'n',			/**< 241 - LATIN SMALL LETTER N WITH TILDE */
	'o',			/**< 242 - LATIN SMALL LETTER O WITH GRAVE */
	'o',			/**< 243 - LATIN SMALL LETTER O WITH ACUTE */
	'o',			/**< 244 - LATIN SMALL LETTER O WITH CIRCUMFLEX */
	'i',			/**< 245 - DOTLESS i */
	'o',			/**< 246 - LATIN SMALL LETTER O WITH DIAERESIS */
	' ',			/**< 247 - SMALL TILDE */
	'o',			/**< 248 - LATIN SMALL LETTER O WITH STROKE */
	' ',			/**< 249 - SEMI-CIRCULAR ACCENT */
	'u',			/**< 250 - LATIN SMALL LETTER U WITH ACUTE */
	'u',			/**< 251 - LATIN SMALL LETTER U WITH CIRCUMFLEX */
	'u',			/**< 252 - LATIN SMALL LETTER U WITH DIAERESIS */
	' ',			/**< 253 - DOUBLE BACKTICK */
	' ',			/**< 254 - CEDILLA */
	'y',			/**< 255 - LATIN SMALL LETTER Y WITH DIAERESIS */
};

/**
 * Describes special files which are served by GTKG.
 */
struct special_file {
	const gchar *path;			/* URL path */
	const gchar *file;			/* File name to load from disk */
	enum share_mime_type type;	/* MIME type of the file */
	const gchar *what;			/* Description of the file for traces */
};

static struct special_file specials[] = {
	{ "/favicon.ico",
			"favicon.png",	SHARE_M_IMAGE_PNG,	"Favorite web icon" },
	{ "/robots.txt",
			"robots.txt",	SHARE_M_TEXT_PLAIN,	"Robot exclusion" },
};

/**
 * Maps special names (e.g. "/favicon.ico") to the shared_file_t structure.
 */
static GHashTable *special_names = NULL;

static guint64 files_scanned = 0;
static guint64 kbytes_scanned = 0;
static guint64 bytes_scanned = 0;

GSList *extensions = NULL;
GSList *shared_dirs = NULL;
static GSList *shared_files = NULL;
static struct shared_file **file_table = NULL;
static search_table_t search_table;
static GHashTable *file_basenames = NULL;

static gchar stmp_1[4096];

/***
 *** Callbacks
 ***/

static listeners_t search_request_listeners = NULL;

void
share_add_search_request_listener(search_request_listener_t l)
{
    LISTENER_ADD(search_request, l);
}

void
share_remove_search_request_listener(search_request_listener_t l)
{
    LISTENER_REMOVE(search_request, l);
}

static void
share_emit_search_request(
	query_type_t type, const gchar *query, const host_addr_t addr, guint16 port)
{
    LISTENER_EMIT(search_request, (type, query, addr, port));
}

/* ----------------------------------------- */

/**
 * A query context.
 *
 * We don't want to include the same file several times in a reply (for
 * example, once because it matches an URN query and once because the file name
 * matches). So we keep track of what has been added in `found_indices'.
 * The file index is used as the key.
 */
struct query_context {
	GHashTable *found_indices;
	GSList *files;				/**< List of shared_file_t that match */
	gint found;
};

/**
 * Create new query context.
 */
static struct query_context *
share_query_context_make(void)
{
	struct query_context *ctx;

	ctx = walloc(sizeof(*ctx));
	ctx->found_indices = g_hash_table_new(NULL, NULL);	/**< direct hashing */
	ctx->files = NULL;
	ctx->found = 0;

	return ctx;
}

/**
 * Get rid of the query context.
 */
static void
share_query_context_free(struct query_context *ctx)
{
	/*
	 * Don't free the `files' list, as we passed it to the query hit builder.
	 */

	g_hash_table_destroy(ctx->found_indices);
	wfree(ctx, sizeof(*ctx));
}

/**
 * Check if a given shared_file has been added to the QueryHit.
 *
 * @return TRUE if the shared_file is in the QueryHit already, FALSE otherwise
 */
static inline gboolean
shared_file_already_found(struct query_context *ctx, const shared_file_t *sf)
{
	return NULL != g_hash_table_lookup(ctx->found_indices,
		GUINT_TO_POINTER(sf->file_index));
}

/**
 * Add the shared_file to the set of files already added to the QueryHit.
 */
static inline void
shared_file_mark_found(struct query_context *ctx, const shared_file_t *sf)
{
	g_hash_table_insert(ctx->found_indices,
		GUINT_TO_POINTER(sf->file_index), GUINT_TO_POINTER(0x1));
}

/**
 * Invoked for each new match we get.
 */
static void
got_match(gpointer context, shared_file_t *sf)
{
	struct query_context *qctx = (struct query_context *) context;

	g_assert(sf->fi == NULL);	/* Cannot match partially downloaded files */

	/*
	 * Don't insert duplicates (possible when matching both by SHA1 and name).
	 */

	if (shared_file_already_found(qctx, sf))
		return;

	shared_file_mark_found(qctx, sf);

	qctx->files = g_slist_prepend(qctx->files, shared_file_ref(sf));
	qctx->found++;
}

/* ----------------------------------------- */

#define FILENAME_CLASH 0xffffffff			/**< Indicates basename clashes */

static char_map_t query_map;

/**
 * Set up keymapping table for Gnutella.
 */
static void
setup_char_map(char_map_t map)
{
	const gchar *charset = locale_get_charset();
	gint c;

	for (c = 0; c < 256; c++)	{
		if (!isupper(c)) {  /* not same than islower, cf ssharp */
			map[c] = tolower(toupper(c)); /* not same than c, cf ssharp */
			map[toupper(c)] = c;
		} else if (isupper(c)) {
			/* handled by previous case */
		} else if (ispunct(c) || isspace(c)) {
			map[c] = ' ';
		} else if (isdigit(c)) {
			map[c] = c;
		} else if (isalnum(c)) {
			map[c] = c;
		} else {
			map[c] = ' ';			/* unknown in our locale */
		}
	}

	if (locale_is_latin()) {
		gboolean b_iso_8859_1 = FALSE;
		gboolean b_cp1252 = FALSE;
		gboolean b_macroman = FALSE;

		if (
				0 == strcmp(charset, "ISO-8859-1") ||
				0 == strcmp(charset, "ISO-8859-15")
		   ) {
			b_iso_8859_1 = TRUE;
		} else if (0 == strcmp(charset, "CP1252")) {
			b_cp1252 = TRUE;
		} else if (0 == strcmp(charset, "MacRoman")) {
			b_macroman = TRUE;
		}

		if (b_iso_8859_1 || b_cp1252) {
			for (c = 160; c < 256; c++)
				map[c] = iso_8859_1[c - 160];
		}
		if (b_cp1252) {
			for (c = 130; c < 160; c++)
				map[c] = cp1252[c - 130];
		} else if (b_macroman) {
			for (c = 130; c < 256; c++)
				map[c] = macroman[c - 130];
		}
	}
}

/**
 * Apply the proper charset mapping on the query, depending on their
 * locale, so that the query has no accent.
 */
void
use_map_on_query(gchar *query, int len)
{
	query += len - 1;
	for (/* empty */; len > 0; len--) {
		*query = query_map[(guchar) *query];
		query--;
	}
}

/* ----------------------------------------- */

/**
 * Initialize special file entry, returning shared_file_t structure if
 * the file exists, NULL otherwise.
 */
static shared_file_t *
share_special_load(struct special_file *sp)
{
	FILE *f;
	gint idx;
	gchar *filename;
	shared_file_t *sf;

#ifndef OFFICIAL_BUILD
	file_path_t fp[3];
#else
	file_path_t fp[2];
#endif

	file_path_set(&fp[0], settings_config_dir(), sp->file);
	file_path_set(&fp[1], PRIVLIB_EXP, sp->file);
#ifndef OFFICIAL_BUILD
	file_path_set(&fp[2], PACKAGE_EXTRA_SOURCE_DIR, sp->file);
#endif

	f = file_config_open_read_norename_chosen(
			sp->what, fp, G_N_ELEMENTS(fp), &idx);

	if (!f)
		return NULL;

	filename = make_pathname(fp[idx].dir, fp[idx].name);

	/*
	 * Create fake special file sharing structure, so that we can
	 * upload it if requested.
	 */

	sf = walloc0(sizeof *sf);
	sf->file_path = atom_str_get(filename);
	sf->name_nfc = atom_str_get(sp->file);		/* ASCII is UTF-8 */
	sf->name_canonic = atom_str_get(sp->file);
	sf->name_nfc_len = strlen(sf->name_nfc);
	sf->name_canonic_len = strlen(sf->name_canonic);
	sf->content_type = share_mime_type(sp->type);

	G_FREE_NULL(filename);
	fclose(f);

	return sf;
}

/**
 * Initialize the special files we're sharing.
 */
static void
share_special_init(void)
{
	guint i;

	special_names = g_hash_table_new(g_str_hash, g_str_equal);

	for (i = 0; i < G_N_ELEMENTS(specials); i++) {
		shared_file_t *sf = share_special_load(&specials[i]);
		if (sf != NULL)
			g_hash_table_insert(special_names,
				deconstify_gchar(specials[i].path), sf);
	}
}

/**
 * Look up a possibly shared special file, updating the entry with current
 * file size and modification time.
 *
 * @param path	the URL path on the server (case sensitive, of course)
 *
 * @return the shared file information if there is something shared at path,
 * or NULL if the path is invalid.
 */
shared_file_t *
shared_special(const gchar *path)
{
	shared_file_t *sf;
	struct stat file_stat;

	sf = g_hash_table_lookup(special_names, path);

	if (sf == NULL)
		return NULL;

	if (-1 == stat(sf->file_path, &file_stat)) {
		g_warning("can't stat %s: %s", sf->file_path, g_strerror(errno));
		return NULL;
	}

	if (!S_ISREG(file_stat.st_mode)) {
		g_warning("file %s is no longer a plain file", sf->file_path);
		return NULL;
	}

	/*
	 * Update information in case the file changed since the last time
	 * we served it.
	 */

	sf->file_size = file_stat.st_size;
	sf->mtime = file_stat.st_mtime;

	return sf;
}

/**
 * Initialization of the sharing library.
 */
void
share_init(void)
{
	setup_char_map(query_map);
	huge_init();
	st_initialize(&search_table, query_map);
	qrp_init(query_map);
	qhit_init();
	oob_init();
	oob_proxy_init();
	share_special_init();

	/**
	 * We allocate an empty search_table, which will be de-allocated when we
	 * call share_scan().  Why do we do this?  Because it ensures the table
	 * is correctly setup empty, until we do call share_scan() for the first
	 * time (the call is delayed until the GUI is up).
	 *
	 * Since we will start processing network packets, we will have a race
	 * condition window if we get a Query message before having started
	 * the share_scan().  Creating the table right now prevents adding an
	 * extra test at the top of st_search().
	 *		--RAM, 15/08/2002.
	 */

	st_create(&search_table);
}

/**
 * Given a valid index, returns the `struct shared_file' entry describing
 * the shared file bearing that index if found, NULL if not found (invalid
 * index) and SHARE_REBUILDING when we're rebuilding the library.
 */
shared_file_t *
shared_file(guint idx)
{
	/* @return shared file info for index `idx', or NULL if none */

	if (file_table == NULL)			/* Rebuilding the library! */
		return SHARE_REBUILDING;

	if (idx < 1 || idx > files_scanned)
		return NULL;

	return file_table[idx - 1];
}

/**
 * Get index of shared file indentified by its name.
 * @return index > 0 if found, 0 if file is not known.
 */
static guint
shared_file_get_index(const gchar *basename)
{
	guint idx;

	idx = GPOINTER_TO_UINT(g_hash_table_lookup(file_basenames, basename));
	if (idx == 0 || idx == FILENAME_CLASH)
		return 0;

	g_assert(idx >= 1 && idx <= files_scanned);
	return idx;	
}

/**
 * Given a file basename, returns the `struct shared_file' entry describing
 * the shared file bearing that basename, provided it is unique, NULL if
 * we either don't have a unique filename or SHARE_REBUILDING if the library
 * is being rebuilt.
 */
shared_file_t *
shared_file_by_name(const gchar *basename)
{
	guint idx;

	if (file_table == NULL)
		return SHARE_REBUILDING;

	g_assert(file_basenames);
	idx = shared_file_get_index(basename);
	return idx == 0 ? NULL : file_table[idx - 1];
}

/**
 * Returns the MIME content type string.
 */
const gchar *
share_mime_type(enum share_mime_type type)
{
	switch (type) {
	case SHARE_M_APPLICATION_BINARY:	return "application/binary";
	case SHARE_M_IMAGE_PNG:				return "image/png";
	case SHARE_M_TEXT_PLAIN:			return "text/plain";
	}

	g_error("unknown MIME type %d", (gint) type);
	return NULL;
}

/* ----------------------------------------- */

/**
 * Free existing extensions
 */
static void
free_extensions(void)
{
	GSList *sl = extensions;

	if (!sl)
		return;

	for ( /*empty */ ; sl; sl = g_slist_next(sl)) {
		struct extension *e = (struct extension *) sl->data;
		atom_str_free(e->str);
		g_free(e);
	}
	g_slist_free(extensions);
	extensions = NULL;
}

/**
 * Get the file extensions to scan.
 */
void
parse_extensions(const gchar *str)
{
	gchar **exts = g_strsplit(str, ";", 0);
	gchar *x, *s;
	guint i;

	free_extensions();

	for (i = 0; exts[i]; i++) {
		gchar c;

		s = exts[i];
		while ((c = *s) == '.' || c == '*' || c == '?' || is_ascii_blank(c))
			s++;

		if (c) {

			for (x = strchr(s, '\0'); x-- != s; /* NOTHING */) {
				if ((c = *x) == '*' || c == '?' || is_ascii_blank(c))
					*x = '\0';
				else
					break;
			}

			if (*s) {
				struct extension *e = g_malloc(sizeof *e);
				e->str = atom_str_get(s);
				e->len = strlen(e->str);
				extensions = g_slist_prepend(extensions, e);
			}
		}
	}

	extensions = g_slist_reverse(extensions);
	g_strfreev(exts);
}

/**
 * Release shared dirs.
 */
static void
shared_dirs_free(void)
{
	GSList *sl;

	if (!shared_dirs)
		return;

	for (sl = shared_dirs; sl; sl = g_slist_next(sl)) {
		atom_str_free(sl->data);
	}
	g_slist_free(shared_dirs);
	shared_dirs = NULL;
}

/**
 * Update the property holding the shared directories.
 */
void
shared_dirs_update_prop(void)
{
    GSList *sl;
    GString *s;

    s = g_string_new("");

    for (sl = shared_dirs; sl != NULL; sl = g_slist_next(sl)) {
        g_string_append(s, sl->data);
        if (g_slist_next(sl) != NULL)
            g_string_append(s, ":");
    }

    gnet_prop_set_string(PROP_SHARED_DIRS_PATHS, s->str);

    g_string_free(s, TRUE);
}

/**
 * Parses the given string and updated the internal list of shared dirs.
 * The given string was completely parsed, it returns TRUE, otherwise
 * it returns FALSE.
 */
gboolean
shared_dirs_parse(const gchar *str)
{
	gchar **dirs = g_strsplit(str, ":", 0);
	guint i = 0;
    gboolean ret = TRUE;

	shared_dirs_free();

	while (dirs[i]) {
		if (is_directory(dirs[i]))
			shared_dirs = g_slist_prepend(shared_dirs, atom_str_get(dirs[i]));
        else
            ret = FALSE;
		i++;
	}

	shared_dirs = g_slist_reverse(shared_dirs);
	g_strfreev(dirs);

    return ret;
}

/**
 * Add directory to the list of shared directories.
 */
void
shared_dir_add(const gchar *path)
{
	if (is_directory(path))
        shared_dirs = g_slist_append(shared_dirs, atom_str_get(path));

    shared_dirs_update_prop();
}

/**
 * Dispose of a shared_file_t structure.
 */
static void
shared_file_free(shared_file_t *sf)
{
	g_assert(sf != NULL);
	g_assert(sf->refcnt == 0);

	atom_str_free(sf->file_path);
	atom_str_free(sf->name_nfc);
	atom_str_free(sf->name_canonic);
	wfree(sf, sizeof *sf);
}

/**
 * Add one more reference to a shared_file_t.
 * @return its argument, for convenience.
 */
shared_file_t *
shared_file_ref(shared_file_t *sf)
{
	sf->refcnt++;
	return sf;
}

/**
 * Remove one reference to a shared_file_t, freeing entry if there are
 * no reference left.
 */
void
shared_file_unref(shared_file_t *sf)
{
	g_assert(sf->refcnt > 0);

	if (--sf->refcnt == 0)
		shared_file_free(sf);
}

/**
 * Is file too big to be shared on Gnutella?
 */
static inline gboolean
too_big_for_gnutella(off_t size)
{
	g_return_val_if_fail(size >= 0, TRUE);
	if (sizeof(off_t) <= sizeof(guint32))
		return FALSE;
	return (guint64) size > ((guint64) 1U << 63) - 1;
}

/**
 * Checks whether it's OK to share the pathname with respect to special
 * characters in the string. As the database stores records line-by-line,
 * newline characters in the filename are not acceptable.
 *
 * @return	If the pathname contains ASCII control characters, TRUE is
 *			returned. Otherwise, the pathname is considered OK and FALSE
 *			is returned.
 */
static gboolean
contains_control_chars(const gchar *pathname)
{
	const gchar *s;

	for (s = pathname; *s != '\0'; s++) {
		if (is_ascii_cntrl(*s))
			return TRUE;
	}
	return FALSE;
}

/**
 * The directories that are given as shared will be completly transversed
 * including all files and directories. An entry of "/" would search the
 * the whole file system.
 */
static void
recurse_scan(const gchar *dir, const gchar *basedir)
{
	GSList *exts = NULL;
	DIR *directory;			/* Dir stream used by opendir, readdir etc.. */
	struct dirent *dir_entry;
	GSList *files = NULL;
	GSList *directories = NULL;
	GSList *sl;
	guint i;
	struct stat file_stat;
	const gchar *entry_end;

	if (*dir == '\0')
		return;

	if (!(directory = opendir(dir))) {
		g_warning("can't open directory %s: %s", dir, g_strerror(errno));
		return;
	}

	while ((dir_entry = readdir(directory))) {
		gchar *full;

		if (dir_entry->d_name[0] == '.')	/* Hidden file, or "." or ".." */
			continue;

		full = make_pathname(dir, dir_entry->d_name);

		if (is_directory(full)) {
			if (scan_ignore_symlink_dirs && is_symlink(full)) {
				G_FREE_NULL(full);
				continue;
			}
			directories = g_slist_prepend(directories, full);
		} else {
			if (scan_ignore_symlink_regfiles && is_symlink(full)) {
				G_FREE_NULL(full);
				continue;
			}
			files = g_slist_prepend(files, full);
		}
	}

	for (i = 0, sl = files; sl; i++, sl = g_slist_next(sl)) {
		const gchar *full, *name;

		full = sl->data;

		/*
		 * In the "tmp" directory, don't share files that have a trailer.
		 * It's probably a file being downloaded, and which is not complete yet.
		 * This check is necessary in case they choose to share their
		 * downloading directory...
		 */

		name = strrchr(full, G_DIR_SEPARATOR);
		g_assert(name && G_DIR_SEPARATOR == name[0]);
		name++;						/* Start of file name */

		entry_end = &name[strlen(name)];

		for (exts = extensions; exts; exts = exts->next) {
			struct extension *e = exts->data;
			const gchar *start = entry_end - (e->len + 1);	/* +1 for "." */

			/*
			 * Look for the trailing chars (we're matching an extension).
			 * Matching is case-insensitive, and the extension opener is ".".
			 *
			 * An extension "--all--" matches all files, even if they
			 * don't have any extension. [Patch from Zygo Blaxell].
			 */

			if (
				0 == ascii_strcasecmp("--all--", e->str) ||	/* All files */
				(start >= name && *start == '.' &&
					0 == ascii_strcasecmp(start + 1, e->str))
			) {
				struct shared_file *found = NULL;
				gchar *q;

				if (share_debug > 5)
					g_message("recurse_scan: full=\"%s\"", full);

				if (contains_control_chars(full)) {
					g_warning("Not sharing filename with control characters: "
						"\"%s\"", full);
					break;
				}

				if (stat(full, &file_stat) == -1) {
					g_warning("can't stat %s: %s", full, g_strerror(errno));
					break;
				}

				if (0 == file_stat.st_size) {
					if (share_debug > 5)
						g_warning("Not sharing empty file: \"%s\"", full);
					break;
				}

				if (!S_ISREG(file_stat.st_mode)) {
					g_warning("Not sharing non-regular file: \"%s\"", full);
					break;
				}

				if (too_big_for_gnutella(file_stat.st_size)) {
					g_warning("File is too big to be shared: \"%s\"", full);
					break;
				}

				found = walloc0(sizeof *found);

				found->file_path = atom_str_get(full);

				/*
				 * Explicitely NFC for better inter-vendor support
				 * and because it's tighter.
				 */
				q = filename_to_utf8_normalized(name, UNI_NORM_NETWORK);
				found->name_nfc = atom_str_get(q);
				if (q != name)
					G_FREE_NULL(q);

				q = UNICODE_CANONIZE(found->name_nfc);
				found->name_canonic = atom_str_get(q);
				if (q != found->name_nfc)
					G_FREE_NULL(q);

#if 0
				printf("\npath=\"%s\"\nnfc=\"%s\"\ncanonic=\"%s\"\n",
					found->file_path, found->name_nfc, found->name_canonic);
#endif

				found->name_nfc_len = strlen(found->name_nfc);
				found->name_canonic_len = strlen(found->name_canonic);

				found->file_size = file_stat.st_size;
				found->file_index = ++files_scanned;
				found->mtime = file_stat.st_mtime;
				found->flags = 0;
				found->content_type =
					share_mime_type(SHARE_M_APPLICATION_BINARY);

				if (0 == found->name_nfc_len || 0 == found->name_canonic_len) {
					g_warning(
						"Normalized filename is an empty string \"%s\" "
						"(NFC=\"%s\", canonic=\"%s\")",
						full, found->name_nfc, found->name_canonic);
					shared_file_free(found);
					found = NULL;
					break;
				}

				if (!sha1_is_cached(found) && file_info_has_trailer(full)) {
					/*
	 		 	 	 * It's probably a file being downloaded, and which is
					 * not complete yet. This check is necessary in case
					 * they choose to share their downloading directory...
	 		  	 	 */

					g_warning("will not share partial file \"%s\"", full);
					shared_file_free(found);
					found = NULL;
					break;
				}

				if (request_sha1(found)) {
					st_insert_item(&search_table, found->name_canonic, found);
					shared_files = g_slist_prepend(shared_files,
							shared_file_ref(found));

					bytes_scanned += file_stat.st_size;
					kbytes_scanned += bytes_scanned >> 10;
					bytes_scanned &= (1 << 10) - 1;
				} else {
					found = NULL;
				}
				break;			/* for loop */
			}
		}
		G_FREE_NULL(sl->data);

		if (!(i & 0x3f)) {
			gcu_gui_update_files_scanned();	/* Interim view */
			gcu_gtk_main_flush();
		}
	}

	closedir(directory);
	directory = NULL;
	g_slist_free(files);
	files = NULL;

	/*
	 * Now that we handled files at this level and freed all their memory,
	 * recurse on directories.
	 */

	for (sl = directories; sl; sl = g_slist_next(sl)) {
		gchar *path = sl->data;
		recurse_scan(path, basedir);
		G_FREE_NULL(path);
	}
	g_slist_free(directories);

	gcu_gui_update_files_scanned();		/* Interim view */
	gcu_gtk_main_flush();
}

/**
 * Free up memory used by the shared library.
 */
static void
share_free(void)
{
	GSList *sl;

	st_destroy(&search_table);

	if (file_basenames) {
		g_hash_table_destroy(file_basenames);
		file_basenames = NULL;
	}

	if (file_table)
		G_FREE_NULL(file_table);

	for (sl = shared_files; sl; sl = g_slist_next(sl)) {
		struct shared_file *sf = sl->data;
		shared_file_unref(sf);
	}

	g_slist_free(shared_files);
	shared_files = NULL;
}

static void reinit_sha1_table(void);

/**
 * Perform scanning of the shared directories to build up the list of
 * shared files.
 */
void
share_scan(void)
{
	GSList *dirs;
	GSList *sl;
	gint i;
	static gboolean in_share_scan = FALSE;
	time_t now, started;
	glong elapsed;

	/*
	 * We normally disable the "Rescan" button, so we should not enter here
	 * twice.  Nonetheless, the events can be stacked, and since we call
	 * the main loop whilst scanning, we could re-enter here.
	 *
	 *		--RAM, 05/06/2002 (added after the above indeed happened)
	 */

	if (in_share_scan)
		return;
	else
		in_share_scan = TRUE;

	started = now = tm_time();

	gnet_prop_set_boolean_val(PROP_LIBRARY_REBUILDING, TRUE);
	gnet_prop_set_timestamp_val(PROP_LIBRARY_RESCAN_STARTED, now);

	files_scanned = 0;
	bytes_scanned = 0;
	kbytes_scanned = 0;

	reinit_sha1_table();
	share_free();

	g_assert(file_basenames == NULL);

	st_create(&search_table);
	file_basenames = g_hash_table_new(g_str_hash, g_str_equal);

	/*
	 * Clone the `shared_dirs' list so that we don't behave strangely
	 * should they update the list of shared directories in the GUI
	 * whilst we're recursing!
	 *		--RAM, 30/01/2003
	 */

	for (dirs = NULL, sl = shared_dirs; sl; sl = g_slist_next(sl))
		dirs = g_slist_prepend(dirs, atom_str_get(sl->data));

	dirs = g_slist_reverse(dirs);

	/* Recurse on the cloned list... */
	for (sl = dirs; sl; sl = g_slist_next(sl))
		recurse_scan(sl->data, sl->data);	/* ...since this updates the GUI! */

	for (sl = dirs; sl; sl = g_slist_next(sl))
		atom_str_free(sl->data);

	g_slist_free(dirs);
	dirs = NULL;

	/*
	 * Done scanning all the files.
	 */

	st_compact(&search_table);

	/*
	 * In order to quickly locate files based on indicies, build a table
	 * of all shared files.  This table is only accessible via shared_file().
	 * NB: file indicies start at 1, but indexing in table start at 0.
	 *		--RAM, 08/10/2001
	 *
	 * We over-allocate the file_table by one entry so that even when they
	 * don't share anything, the `file_table' pointer is not NULL.
	 * This will prevent us giving back "rebuilding library" when we should
	 * actually return "not found" for user download requests.
	 *		--RAM, 23/10/2002
	 */

	file_table = g_malloc0((files_scanned + 1) * sizeof *file_table);

	for (i = 0, sl = shared_files; sl; i++, sl = g_slist_next(sl)) {
		struct shared_file *sf = sl->data;
		guint val;

		g_assert(sf->file_index > 0 && sf->file_index <= files_scanned);
		file_table[sf->file_index - 1] = sf;

		/*
		 * In order to transparently handle files requested with the wrong
		 * indices, for older servents that would not know how to handle a
		 * return code of "301 Moved" with a Location header, we keep track
		 * of individual basenames of files, recording the index of each file.
		 * As soon as there is a clash, we revoke the entry by storing
		 * FILENAME_CLASH instead, which cannot be a valid index.
		 *		--RAM, 06/06/2002
		 */

		val = GPOINTER_TO_UINT(
			g_hash_table_lookup(file_basenames, sf->name_nfc));

		/*
		 * The following works because 0 cannot be a valid file index.
		 */

		val = (val != 0) ? FILENAME_CLASH : sf->file_index;
		g_hash_table_insert(file_basenames, deconstify_gchar(sf->name_nfc),
			GUINT_TO_POINTER(val));

		if (0 == (i & 0x7ff))
			gcu_gtk_main_flush();
	}

	gcu_gui_update_files_scanned();		/* Final view */

	now = tm_time();
	elapsed = delta_time(now, started);
	elapsed = MAX(0, elapsed);
	gnet_prop_set_timestamp_val(PROP_LIBRARY_RESCAN_FINISHED, now);
	gnet_prop_set_guint32_val(PROP_LIBRARY_RESCAN_DURATION, elapsed);

	/*
	 * Query routing table update.
	 */

	started = now;
	gnet_prop_set_timestamp_val(PROP_QRP_INDEXING_STARTED, now);

	qrp_prepare_computation();

	for (i = 0, sl = shared_files; sl; i++, sl = g_slist_next(sl)) {
		struct shared_file *sf = sl->data;
		qrp_add_file(sf);
		if (0 == (i & 0x7ff))
			gcu_gtk_main_flush();
	}

	qrp_finalize_computation();

	now = tm_time();
	elapsed = delta_time(now, started);
	elapsed = MAX(0, elapsed);
	gnet_prop_set_guint32_val(PROP_QRP_INDEXING_DURATION, elapsed);

	in_share_scan = FALSE;
	gnet_prop_set_boolean_val(PROP_LIBRARY_REBUILDING, FALSE);
}

/**
 * Hash table iterator callback to free the value.
 */
static void
special_free_kv(gpointer unused_key, gpointer val, gpointer unused_udata)
{
	(void) unused_key;
	(void) unused_udata;

	shared_file_free(val);
}

/**
 * Get rid of the special file descriptions, if any.
 */
static void
share_special_close(void)
{
	g_hash_table_foreach(special_names, special_free_kv, NULL);
	g_hash_table_destroy(special_names);
}

/**
 * Shutdown cleanup.
 */
void
share_close(void)
{
	share_special_close();
	free_extensions();
	share_free();
	shared_dirs_free();
	huge_close();
	qrp_close();
	oob_proxy_close();
	oob_close();
	qhit_close();
}

#define MIN_WORD_LENGTH 1		/**< For compaction */

/**
 * Remove unnecessary ballast from a query before processing it. Works in
 * place on the given string. Removed are all consecutive blocks of
 * whitespace and all words shorter then MIN_WORD_LENGTH.
 *
 * @param search	the search string to compact, modified in place.
 * @return			the length in bytes of the compacted search string.
 */
static size_t
compact_query_utf8(gchar *search)
{
	gchar *s;
	gchar *word = NULL, *p;
	size_t word_length = 0;	/* length in bytes, not characters */

#define APPEND_WORD()								\
do {												\
	/* Append a space unless it's the first word */	\
	if (p != search) {								\
		if (*p != ' ')								\
			*p = ' ';								\
		p++;										\
	}												\
	if (p != word)									\
		memmove(p, word, word_length);				\
	p += word_length;								\
} while (0)

	if (share_debug > 4)
		g_message("original: [%s]", search);

	word = is_ascii_blank(*search) ? NULL : search;
	p = s = search;
	while ('\0' != *s) {
		guint clen;

		clen = utf8_char_len(s);
		clen = MAX(1, clen);	/* In case of invalid UTF-8 */

		if (is_ascii_blank(*s)) {
			if (word_length >= MIN_WORD_LENGTH) {
				APPEND_WORD();
			}
			word_length = 0;

			s = skip_ascii_blanks(s);
			if ('\0' == *s) {
				word = NULL;
				break;
			}
			word = s;
		} else {
			word_length += clen;
			s += clen;
		}
	}

	if (word_length >= MIN_WORD_LENGTH) {
		APPEND_WORD();
	}

	if ('\0' != *p)
		*p = '\0'; /* terminate mangled query */

	if (share_debug > 4)
		g_message("mangled: [%s]", search);

	/* search does no longer contain unnecessary whitespace */
	return p - search;
}

/**
 * Determine whether the given string is UTF-8 encoded.
 * If query starts with a BOM mark, skip it and set `retoff' accordingly.
 *
 * @returns TRUE if the string is valid UTF-8, FALSE otherwise.
 */
static gboolean 
query_utf8_decode(const gchar *text, guint *retoff)
{
	const gchar *p;

	/*
	 * Look whether we're facing an UTF-8 query.
	 *
	 * If it starts with the sequence EF BB BF (BOM in UTF-8), then
	 * it is clearly UTF-8.  If we can't decode it, it is bad UTF-8.
	 */

	if (!(p = is_strprefix(text, "\xef\xbb\xbf")))
		p = text;
	
	if (retoff)
		*retoff = p - text;

	/* Disallow BOM followed by an empty string */	
	return (p == text || '\0' != p[0]) && utf8_is_valid_string(p);
}

/**
 * Remove unnecessary ballast from a query string, in-place.
 *
 * @returns new query string length.
 */
size_t
compact_query(gchar *search)
{
	size_t mangled_search_len, orig_len = strlen(search);
	guint offset;			/* Query string start offset */

	/*
	 * Look whether we're facing an UTF-8 query.
	 */

	if (!query_utf8_decode(search, &offset))
		g_error("found invalid UTF-8 after a leading BOM");

	/*
	 * Compact the query, offsetting from the start as needed in case
	 * there is a leading BOM (our UTF-8 decoder does not allow BOM
	 * within the UTF-8 string, and rightly I think: that would be pure
	 * gratuitous bloat).
	 */

	mangled_search_len = compact_query_utf8(&search[offset]);

	g_assert(mangled_search_len <= (size_t) orig_len - offset);

	/*
	 * Get rid of BOM, if any.
	 */

	if (offset > 0)
		memmove(search, &search[offset], mangled_search_len);

	return mangled_search_len;
}

/**
 * Remove the OOB delivery flag by patching the query message inplace.
 */
void
query_strip_oob_flag(gnutella_node_t *n, gchar *data)
{
	guint16 speed;

	READ_GUINT16_LE(data, speed);
	speed &= ~QUERY_SPEED_OOB_REPLY;
	WRITE_GUINT16_LE(speed, data);

	gnet_stats_count_general(GNR_OOB_QUERIES_STRIPPED, 1);

	if (query_debug)
		g_message(
			"QUERY from node %s <%s>: removed OOB delivery (speed = 0x%x)",
			node_addr(n), node_vendor(n), speed);
}

/**
 * Set the OOB delivery flag by patching the query message inplace.
 */
void
query_set_oob_flag(gnutella_node_t *n, gchar *data)
{
	guint16 speed;

	READ_GUINT16_LE(data, speed);
	speed |= QUERY_SPEED_OOB_REPLY | QUERY_SPEED_MARK;
	WRITE_GUINT16_LE(speed, data);

	if (query_debug)
		g_message(
			"QUERY %s from node %s <%s>: set OOB delivery (speed = 0x%x)",
			guid_hex_str(n->header.muid), node_addr(n), node_vendor(n), speed);
}

/**
 * Searches requests (from others nodes)
 * Basic matching. The search request is made lowercase and
 * is matched to the filenames in the LL.
 *
 * If `qhv' is not NULL, it is filled with hashes of URN or query words,
 * so that we may later properly route the query among the leaf nodes.
 *
 * @returns TRUE if the message should be dropped and not propagated further.
 */
gboolean
search_request(struct gnutella_node *n, query_hashvec_t *qhv)
{
	guint16 req_speed;
	gchar *search;
	size_t search_len;
	gboolean decoded = FALSE;
	guint32 max_replies;
	gboolean skip_file_search = FALSE;
	extvec_t exv[MAX_EXTVEC];
	gint exvcnt = 0;
	struct {
		gchar sha1_digest[SHA1_RAW_SIZE];
		gboolean matched;
	} exv_sha1[MAX_EXTVEC];
	gchar *last_sha1_digest = NULL;
	gint exv_sha1cnt = 0;
	guint offset = 0;			/**< Query string start offset */
	gboolean drop_it = FALSE;
	gboolean oob = FALSE;		/**< Wants out-of-band query hit delivery? */
	gboolean use_ggep_h = FALSE;
	struct query_context *qctx;
	gboolean tagged_speed = FALSE;
	gboolean should_oob = FALSE;
	gchar muid[GUID_RAW_SIZE];

	/*
	 * Make sure search request is NUL terminated... --RAM, 06/10/2001
	 *
	 * We can't simply check the last byte, because there can be extensions
	 * at the end of the query after the first NUL.  So we need to scan the
	 * string.  Note that we use this scanning opportunity to also compute
	 * the search string length.
	 *		--RAN, 21/12/2001
	 */

	search = n->data + 2;
	search_len = 0;

	/* open a block, since C doesn't allow variables to be declared anywhere */
	{
		static const gchar qtrax2_con[] = "QTRAX2_CONNECTION";
		gchar *s = search;
		guint32 max_len = n->size - 3;		/* Payload size - Speed - NUL */

        while (search_len <= max_len && *s++)
            search_len ++;

		if (search_len > max_len) {
			g_assert(n->data[n->size - 1] != '\0');
			if (share_debug)
				g_warning("query (hops=%d, ttl=%d) had no NUL (%d byte%s)",
					n->header.hops, n->header.ttl, n->size - 2,
					n->size == 3 ? "" : "s");
			if (share_debug > 4)
				dump_hex(stderr, "Query Text", search, MIN(n->size - 2, 256));

            gnet_stats_count_dropped(n, MSG_DROP_QUERY_NO_NUL);
			return TRUE;		/* Drop the message! */
		}
		/* We can now use `search' safely as a C string: it embeds a NUL */

		/*
		 * Drop the "QTRAX2_CONNECTION" queries as being "overhead".
		 */

		if (
			search_len >= CONST_STRLEN(qtrax2_con) &&
			is_strprefix(search, qtrax2_con)
		) {
            gnet_stats_count_dropped(n, MSG_DROP_QUERY_OVERHEAD);
			return TRUE;		/* Drop the message! */
		}
    }

	/*
	 * Compact query, if requested and we're going to relay that message.
	 */

	if (
		gnet_compact_query &&
		n->header.ttl &&
		current_peermode != NODE_P_LEAF
	) {
		size_t mangled_search_len;

		/*
		 * Look whether we're facing an UTF-8 query.
		 */

		if (!query_utf8_decode(search, &offset)) {
			gnet_stats_count_dropped(n, MSG_DROP_MALFORMED_UTF_8);
			return TRUE;					/* Drop message! */
		}
		decoded = TRUE;

		if (!is_ascii_string(search))
			gnet_stats_count_general(GNR_QUERY_UTF8, 1);

		/*
		 * Compact the query, offsetting from the start as needed in case
		 * there is a leading BOM (our UTF-8 decoder does not allow BOM
		 * within the UTF-8 string, and rightly I think: that would be pure
		 * gratuitous bloat).
		 */

		mangled_search_len = compact_query_utf8(&search[offset]);

		g_assert(mangled_search_len <= search_len - offset);

		if (mangled_search_len != search_len - offset) {
			gnet_stats_count_general(GNR_QUERY_COMPACT_COUNT, 1);
			gnet_stats_count_general(GNR_QUERY_COMPACT_SIZE,
				search_len - offset - mangled_search_len);
		}

		/*
		 * Need to move the trailing data forward and adjust the
		 * size of the packet.
		 */

		g_memmove(
			&search[offset + mangled_search_len], /* new end of query string */
			&search[search_len],                  /* old end of query string */
			n->size - (search - n->data) - search_len); /* trailer len */

		n->size -= search_len - offset - mangled_search_len;
		WRITE_GUINT32_LE(n->size, n->header.size);
		search_len = mangled_search_len + offset;

		g_assert('\0' == search[search_len]);
	}

	/*
	 * If there are extra data after the first NUL, fill the extension vector.
	 */

	if (search_len + 3 != n->size) {
		gint extra = n->size - 3 - search_len;		/* Amount of extra data */
		gint i;

		ext_prepare(exv, MAX_EXTVEC);
		exvcnt = ext_parse(search + search_len + 1, extra, exv, MAX_EXTVEC);

		if (exvcnt == MAX_EXTVEC) {
			g_warning("%s has %d extensions!",
				gmsg_infostr(&n->header), exvcnt);
			if (share_debug)
				ext_dump(stderr, exv, exvcnt, "> ", "\n", TRUE);
			if (share_debug > 1)
				dump_hex(stderr, "Query", search, n->size - 2);
		}

		if (exvcnt && share_debug > 3) {
			g_message("query with extensions: %s\n", search);
			ext_dump(stderr, exv, exvcnt, "> ", "\n", share_debug > 4);
		}

		/*
		 * If there is a SHA1 URN, validate it and extract the binary digest
		 * into sha1_digest[], and set `sha1_query' to the base32 value.
		 */

		for (i = 0; i < exvcnt; i++) {
			extvec_t *e = &exv[i];

			if (e->ext_token == EXT_T_OVERHEAD) {
				if (share_debug > 6)
					dump_hex(stderr, "Query Packet (BAD: has overhead)",
						search, MIN(n->size - 2, 256));
				gnet_stats_count_dropped(n, MSG_DROP_QUERY_OVERHEAD);
				ext_reset(exv, MAX_EXTVEC);
				return TRUE;			/* Drop message! */
			}

			if (e->ext_token == EXT_T_URN_SHA1) {
				gchar *sha1_digest = exv_sha1[exv_sha1cnt].sha1_digest;
				gint paylen = ext_paylen(e);

				if (paylen == 0)
					continue;				/* A simple "urn:sha1:" */

				if (
					!huge_sha1_extract32(ext_payload(e), paylen,
						sha1_digest, &n->header, FALSE)
                ) {
                    gnet_stats_count_dropped(n, MSG_DROP_MALFORMED_SHA1);
					ext_reset(exv, MAX_EXTVEC);
					return TRUE;			/* Drop message! */
                }

				exv_sha1[exv_sha1cnt].matched = FALSE;
				exv_sha1cnt++;

				if (share_debug > 4)
					g_message("valid SHA1 #%d in query: %32s",
						exv_sha1cnt, ext_payload(e));

				/*
				 * Add valid URN query to the list of query hashes, if we
				 * are to fill any for query routing.
				 */

				if (qhv != NULL) {
					gm_snprintf(stmp_1, sizeof(stmp_1),
						"urn:sha1:%s", sha1_base32(sha1_digest));
					qhvec_add(qhv, stmp_1, QUERY_H_URN);
				}

				last_sha1_digest = sha1_digest;
			}
		}

		if (exv_sha1cnt)
			gnet_stats_count_general(GNR_QUERY_SHA1, 1);

		if (exvcnt)
			ext_reset(exv, MAX_EXTVEC);
	}

    /*
     * Reorderd the checks: if we drop the packet, we won't notify any
     * listeners. We first check whether we want to drop the packet and
     * later decide whether we are eligible for answering the query:
     * 1) try top drop
     * 2) notify listeners
     * 3) bail out if not eligible for a local search
     * 4) local search
     *      --Richard, 11/09/2002
     */

	/*
	 * When an URN search is present, there can be an empty search string.
	 *
	 * If requester if farther than half our TTL hops. save bandwidth when
	 * returning lots of hits from short queries, which are not specific enough.
	 * The idea here is to give some response, but not too many.
	 */

	if (
		search_len <= 1 ||
		(search_len < 5 && n->header.hops > (max_ttl / 2))
	)
		skip_file_search = TRUE;

    if (0 == exv_sha1cnt && skip_file_search) {
        gnet_stats_count_dropped(n, MSG_DROP_QUERY_TOO_SHORT);
		return TRUE;					/* Drop this search message */
    }

	/*
	 * When we are not a leaf node, we do two sanity checks here:
	 *
	 * 1. We keep track of all the queries sent by the node (hops = 1)
	 *    and the time by which we saw them.  If they are sent too often,
	 *    just drop the duplicates.  Since an Ultranode will send queries
	 *    from its leaves with an adjusted hop, we only do that for leaf
	 *    nodes.
	 *
	 * 2. We keep track of all queries relayed by the node (hops >= 1)
	 *    by hops and by search text for a limited period of time.
	 *    The purpose is to sanitize the traffic if the node did not do
	 *    point #1 above for its own neighbours.  Naturally, we expire
	 *    this data more quickly.
	 *
	 * When there is a SHA1 in the query, it is the SHA1 itself that is
	 * being remembered.
	 *
	 *		--RAM, 09/12/2003
	 */

	if (n->header.hops == 1 && n->qseen != NULL) {
		time_t now = tm_time();
		time_t seen = 0;
		gboolean found;
		gpointer atom;
		gpointer seenp;
		gchar *query = search;

		g_assert(NODE_IS_LEAF(n));

		if (last_sha1_digest != NULL) {
			gm_snprintf(stmp_1, sizeof(stmp_1),
				"urn:sha1:%s", sha1_base32(last_sha1_digest));
			query = stmp_1;
		}

		found = g_hash_table_lookup_extended(n->qseen, query, &atom, &seenp);
		if (found)
			seen = (time_t) GPOINTER_TO_INT(seenp);

		if (delta_time(now, (time_t) 0) - seen < node_requery_threshold) {
			if (share_debug) g_warning(
				"node %s (%s) re-queried \"%s\" after %d secs",
				node_addr(n), node_vendor(n), query, (gint) (now - seen));
			gnet_stats_count_dropped(n, MSG_DROP_THROTTLE);
			return TRUE;		/* Drop the message! */
		}

		if (!found)
			atom = atom_str_get(query);

		g_hash_table_insert(n->qseen, atom,
			GINT_TO_POINTER((gint) delta_time(now, (time_t) 0)));
	}

	/*
	 * For point #2, there are two tables to consider: `qrelayed_old' and
	 * `qrelayed'.  Presence in any of the tables is sufficient, but we
	 * only insert in the "new" table `qrelayed'.
	 */

	if (n->qrelayed != NULL) {					/* Check #2 */
		gpointer found = NULL;

		g_assert(!NODE_IS_LEAF(n));

		/*
		 * Consider both hops and TTL for dynamic querying, whereby the
		 * same query can be repeated with an increased TTL.
		 */

		if (last_sha1_digest == NULL)
			gm_snprintf(stmp_1, sizeof(stmp_1),
				"%d/%d%s", n->header.hops, n->header.ttl, search);
		else
			gm_snprintf(stmp_1, sizeof(stmp_1),
				"%d/%durn:sha1:%s", n->header.hops, n->header.ttl,
				sha1_base32(last_sha1_digest));

		if (n->qrelayed_old != NULL)
			found = g_hash_table_lookup(n->qrelayed_old, stmp_1);

		if (found == NULL)
			found = g_hash_table_lookup(n->qrelayed, stmp_1);

		if (found != NULL) {
			if (share_debug) g_warning(
				"dropping query \"%s%s\" (hops=%d, TTL=%d) "
				"already seen recently from %s (%s)",
				last_sha1_digest == NULL ? "" : "urn:sha1:",
				last_sha1_digest == NULL ?
					search : sha1_base32(last_sha1_digest),
				n->header.hops, n->header.ttl,
				node_addr(n), node_vendor(n));
			gnet_stats_count_dropped(n, MSG_DROP_THROTTLE);
			return TRUE;		/* Drop the message! */
		}

		g_hash_table_insert(n->qrelayed,
			atom_str_get(stmp_1), GINT_TO_POINTER(1));
	}

    /*
     * Push the query string to interested ones (GUI tracing).
     */

    if (
		(search[0] == '\0' || (search[0] == '\\' && search[1] == '\0'))
		&& exv_sha1cnt
    ) {
		gint i;
		for (i = 0; i < exv_sha1cnt; i++)
			share_emit_search_request(QUERY_SHA1,
				sha1_base32(exv_sha1[i].sha1_digest), n->addr, n->port);
	} else
		share_emit_search_request(QUERY_STRING, search, n->addr, n->port);

	/*
	 * Special processing for the "connection speed" field of queries.
	 *
	 * Unless bit 15 is set, process as a speed.
	 * Otherwise if bit 15 is set:
	 *
	 * 1. If the firewall bit (bit 14) is set, the remote servent is firewalled.
	 *    Therefore, if we are also firewalled, don't reply.
	 *
	 * 2. If the XML bit (bit 13) is cleared and we support XML meta data, don't
	 *    include them in the result set [GTKG does not support XML meta data]
	 *
	 *		--RAM, 19/01/2003, updated 06/07/2003 (bit 14-13 instead of 8-9)
	 *
	 * 3. If the GGEP "H" bit (bit 11) is set, the issuer of the query will
	 *    understand the "H" extension in query hits.
	 *		--RAM, 16/07/2003
	 *
	 * Starting today (06/07/2003), we ignore the connection speed overall
	 * if it's not marked with the QUERY_SPEED_MARK flag to indicate new
	 * interpretation. --RAM
	 */

	READ_GUINT16_LE(n->data, req_speed);

	tagged_speed = (req_speed & QUERY_SPEED_MARK) ? TRUE : FALSE;
	oob = tagged_speed && (req_speed & QUERY_SPEED_OOB_REPLY);
	use_ggep_h = tagged_speed && (req_speed & QUERY_SPEED_GGEP_H);

	/*
	 * If query comes from GTKG 0.91 or later, it understands GGEP "H".
	 * Otherwise, it's an old servent or one unwilling to support this new
	 * extension, so it will get its SHA1 URNs in ASCII form.
	 *		--RAM, 17/11/2002
	 */

	{
		guint8 major, minor;
		gboolean release;

		if (
			guid_query_muid_is_gtkg(n->header.muid, oob,
				&major, &minor, &release)
		) {
			gboolean requery = guid_is_requery(n->header.muid);

			/* Only supersede `use_ggep_h' if not indicated in "min speed" */
			if (!use_ggep_h)
				use_ggep_h =
					major >= 1 || minor > 91 || (minor == 91 && release);

			gnet_stats_count_general(GNR_GTKG_TOTAL_QUERIES, 1);
			if (requery)
				gnet_stats_count_general(GNR_GTKG_REQUERIES, 1);

			if (query_debug > 3)
				g_message("GTKG %s%squery from %d.%d%s",
					oob ? "OOB " : "", requery ? "re-" : "",
					major, minor, release ? "" : "u");
		}
	}

	if (use_ggep_h)
		gnet_stats_count_general(GNR_QUERIES_WITH_GGEP_H, 1);

	/*
	 * If OOB reply is wanted, validate a few things.
	 *
	 * We may either drop the query, or reset the OOB flag if it's
	 * obviously misconfigured.  Then we can re-enable the OOB flag
	 * if we're allowed to perform OOB-proxying for leaf queries.
	 */

	if (oob) {
		host_addr_t addr;
		guint16 port;

		guid_oob_get_addr_port(n->header.muid, &addr, &port);

		/*
		 * Verify against the hostile IP addresses...
		 */

		if (hostiles_check(addr)) {
			gnet_stats_count_dropped(n, MSG_DROP_HOSTILE_IP);
			return TRUE;		/* Drop the message! */
		}

		/*
		 * If it's a neighbouring query, make sure the IP for results
		 * matches what we know about the listening IP for the node.
		 * The UDP port can be different from the TCP port, so we can't
		 * check that.
		 */

		if (	n->header.hops == 1 &&
				is_host_addr(n->gnet_addr) &&
				!host_addr_equal(addr, n->gnet_addr)
		) {
			gnet_stats_count_dropped(n, MSG_DROP_BAD_RETURN_ADDRESS);

			if (query_debug)
				g_message("QUERY dropped from node %s <%s>: invalid OOB flag "
					"(return address mismatch: %s, node: %s)",
					node_addr(n), node_vendor(n),
					host_addr_port_to_string(addr, port), node_gnet_addr(n));

			return TRUE;		/* Drop the message! */
		}

		/*
		 * If the query contains an invalid IP:port, clear the OOB flag.
		 */

		if (!host_is_valid(addr, port)) {
			query_strip_oob_flag(n, n->data);
			oob = FALSE;

			if (query_debug)
				g_message("QUERY %s node %s <%s>: removed OOB flag "
					"(invalid return address: %s)",
					guid_hex_str(n->header.muid), node_addr(n), node_vendor(n),
					host_addr_port_to_string(addr, port));
		}

		/*
		 * If the query comes from a leaf node and has the "firewalled"
		 * bit set, chances are the leaf is UDP-firewalled as well.
		 * Clear the OOB flag.
		 */

		if (oob && NODE_IS_LEAF(n) && (req_speed & QUERY_SPEED_FIREWALLED)) {
			query_strip_oob_flag(n, n->data);
			oob = FALSE;

			if (query_debug)
				g_message("QUERY %s node %s <%s>: removed OOB flag "
					"(leaf node is TCP-firewalled)",
					guid_hex_str(n->header.muid), node_addr(n), node_vendor(n));
		}

		/*
		 * If the leaf node is not guiding the query, yet requests out-of-band
		 * replies, clear that flag so that we can monitor how much hits
		 * are delivered.
		 */

		if (
			oob && NODE_IS_LEAF(n) &&
			!(NODE_GUIDES_QUERY(n) || (req_speed & QUERY_SPEED_LEAF_GUIDED))
		) {
			query_strip_oob_flag(n, n->data);
			oob = FALSE;

			if (query_debug)
				g_message("QUERY %s node %s <%s>: removed OOB flag "
					"(no leaf guidance)",
					guid_hex_str(n->header.muid), node_addr(n), node_vendor(n));
		}
	}

	/*
	 * If the query comes from a node farther than our TTL (i.e. the TTL we'll
	 * use to send our reply), don't bother processing it: the reply won't
	 * be able to reach the issuing node.
	 *
	 * However, note that for replies, we use our maximum configured TTL for
	 * relayed messages, so we compare to that, and not to my_ttl, which is
	 * the TTL used for "standard" packets.
	 *
	 *				--RAM, 12/09/2001
	 *
	 * Naturally, we don't do this check for OOB queries, since the reply
	 * won't be relayed but delivered directly via UDP.
	 *
	 *				--RAM, 2004-11-27
	 */

	should_oob = process_oob_queries && udp_active() &&
		recv_solicited_udp && n->header.hops > 1;

    if (n->header.hops > max_ttl && !(oob && should_oob)) {
        gnet_stats_count_dropped(n, MSG_DROP_MAX_TTL_EXCEEDED);
		return TRUE;					/* Drop this long-lived search */
    }

	/*
	 * If the query does not have an OOB mark, comes from a leaf node and
	 * they allow us to be an OOB-proxy, then replace the IP:port of the
	 * query with ours, so that we are the ones to get the UDP replies.
	 *
	 * Since calling oob_proxy_create() is going to mangle the query's
	 * MUID in place (alterting n->header.muid), we must save the MUID
	 * in case we have local hits to deliver: since we send those directly
	 *		--RAM, 2005-08-28
	 */

	memcpy(muid, n->header.muid, GUID_RAW_SIZE);

	if (
		!oob && udp_active() && proxy_oob_queries && !is_udp_firewalled &&
		NODE_IS_LEAF(n) && host_is_valid(listen_addr(), listen_port)
	) {
		oob_proxy_create(n);
		oob = TRUE;
		gnet_stats_count_general(GNR_OOB_PROXIED_QUERIES, 1);
	}

	if (tagged_speed) {
		if ((req_speed & QUERY_SPEED_FIREWALLED) && is_firewalled)
			return FALSE;			/* Both servents are firewalled */
	}

	/*
	 * Perform search...
	 */

    gnet_stats_count_general(GNR_LOCAL_SEARCHES, 1);
	if (current_peermode == NODE_P_LEAF && node_ultra_received_qrp(n))
		node_inc_qrp_query(n);

	qctx = share_query_context_make();
	max_replies = (search_max_items == (guint32) -1) ? 255 : search_max_items;

	/*
	 * Search each SHA1.
	 */

	if (exv_sha1cnt) {
		gint i;

		for (i = 0; i < exv_sha1cnt && max_replies > 0; i++) {
			struct shared_file *sf;

			sf = shared_file_by_sha1(exv_sha1[i].sha1_digest);
			if (sf && sf != SHARE_REBUILDING && sf->fi == NULL) {
				got_match(qctx, sf);
				max_replies--;
			}
		}
	}

	if (!skip_file_search) {

		/*
		 * Keep only UTF8 encoded queries (This includes ASCII)
		 */

		g_assert('\0' == search[search_len]);

		if (!decoded) {
		   	if (!query_utf8_decode(search, &offset)) {
				gnet_stats_count_dropped(n, MSG_DROP_MALFORMED_UTF_8);
				drop_it = TRUE;				/* Drop message! */
				goto finish;				/* Flush any SHA1 result we have */
			}
			decoded = TRUE;
		
			if (!is_ascii_string(search))
				gnet_stats_count_general(GNR_QUERY_UTF8, 1);
		}

		/*
		 * Because st_search() will apply a character map over the string,
		 * we always need to copy the query string to avoid changing the
		 * data inplace.
		 *
		 * `stmp_1' is a static buffer.  Note that we copy the trailing NUL
		 * into the buffer, hence the "+1" below.
		 */

		search_len -= offset;
		memcpy(stmp_1, &search[offset], search_len + 1);

		st_search(&search_table, stmp_1, got_match, qctx, max_replies, qhv);
	}

finish:
	if (qctx->found > 0) {
        gnet_stats_count_general(GNR_LOCAL_HITS, qctx->found);
		if (current_peermode == NODE_P_LEAF && node_ultra_received_qrp(n))
			node_inc_qrp_match(n);

		if (share_debug > 3) {
			g_message("share HIT %u files '%s'%s ", qctx->found,
				search + offset,
				skip_file_search ? " (skipped)" : "");
			if (exv_sha1cnt) {
				gint i;
				for (i = 0; i < exv_sha1cnt; i++)
					g_message("\t%c(%32s)",
						exv_sha1[i].matched ? '+' : '-',
						sha1_base32(exv_sha1[i].sha1_digest));
			}
			g_message("\treq_speed=%u ttl=%d hops=%d", (guint) req_speed,
				(gint) n->header.ttl, (gint) n->header.hops);
		}
	}

	if (share_debug > 3)
		g_message("QUERY %s \"%s\" has %u hit%s",
			guid_hex_str(n->header.muid), search, qctx->found,
			qctx->found == 1 ? "" : "s");

	/*
	 * If we got a query marked for OOB results delivery, send them
	 * a reply out-of-band but only if the query's hops is > 1.  Otherwise,
	 * we have a direct link to the queryier.
	 */

	if (qctx->found) {
		if (oob && should_oob)
			oob_got_results(n, qctx->files, qctx->found, use_ggep_h);
		else
			qhit_send_results(n, qctx->files, qctx->found, muid, use_ggep_h);
	}

	share_query_context_free(qctx);

	return drop_it;
}

/*
 * SHA1 digest processing
 */

/**
 * This tree maps a SHA1 hash (base-32 encoded) onto the corresponding
 * shared_file if we have one.
 */

static GTree *sha1_to_share = NULL;

/**
 * Compare binary SHA1 hashes.
 * @return 0 if they're the same, a negative or positive number if s1 if greater
 * than s2 or s1 greater than s2, respectively.
 * Used to search the sha1_to_share tree.
 */
static gint
compare_share_sha1(const gchar *s1, const gchar *s2)
{
	return memcmp(s1, s2, SHA1_RAW_SIZE);
}

/**
 * Reset sha1_to_share
 */
static void
reinit_sha1_table(void)
{
	if (sha1_to_share)
		g_tree_destroy(sha1_to_share);

	sha1_to_share = g_tree_new((GCompareFunc) compare_share_sha1);
}

/**
 * Set the SHA1 hash of a given shared_file. Take care of updating the
 * sha1_to_share structure. This function is called from inside the bowels of
 * huge.c when it knows what the hash associated to a file is.
 */
void
set_sha1(struct shared_file *f, const char *sha1)
{
	g_assert(f->fi == NULL);		/* Cannot be a partial file */

	/*
	 * If we were recomputing the SHA1, remove the old version.
	 */

	if (f->flags & SHARE_F_RECOMPUTING) {
		f->flags &= ~SHARE_F_RECOMPUTING;
		g_tree_remove(sha1_to_share, f->sha1_digest);
	}

	memcpy(f->sha1_digest, sha1, SHA1_RAW_SIZE);
	f->flags |= SHARE_F_HAS_DIGEST;
	g_tree_insert(sha1_to_share, f->sha1_digest, f);
}

/**
 * Predicate returning TRUE if the SHA1 hash is available for a given
 * shared_file, FALSE otherwise.
 *
 * Use sha1_hash_is_uptodate() to check for availability and accurateness.
 */
gboolean
sha1_hash_available(const struct shared_file *sf)
{
	return SHARE_F_HAS_DIGEST ==
		(sf->flags & (SHARE_F_HAS_DIGEST | SHARE_F_RECOMPUTING));
}

/**
 * Predicate returning TRUE if the SHA1 hash is available AND is up to date
 * for the shared file.
 *
 * NB: if the file is found to have changed, the background computation of
 * the SHA1 is requested.
 */
gboolean
sha1_hash_is_uptodate(struct shared_file *sf)
{
	struct stat buf;

	if (!(sf->flags & SHARE_F_HAS_DIGEST))
		return FALSE;

	if (sf->flags & SHARE_F_RECOMPUTING)
		return FALSE;

	/*
	 * If there is a non-NULL `fi' entry, then this is a partially
	 * downloaded file that we are sharing.  Don't try to update its
	 * SHA1 by recomputing it!
	 *
	 * If it's a partial file, don't bother checking whether it exists.
	 * (if gone, we won't be able to serve it, that's all).  But partial
	 * files we serve MUST have known SHA1.
	 */

	if (sf->fi != NULL) {
		g_assert(sf->fi->sha1 != NULL);
		return TRUE;
	}

	if (-1 == stat(sf->file_path, &buf)) {
		g_warning("can't stat shared file #%d \"%s\": %s",
			sf->file_index, sf->file_path, g_strerror(errno));
		g_tree_remove(sha1_to_share, sf->sha1_digest);
		sf->flags &= ~SHARE_F_HAS_DIGEST;
		return FALSE;
	}

	if (too_big_for_gnutella(buf.st_size)) {
		g_warning("File is too big to be shared: \"%s\"", sf->file_path);
		g_tree_remove(sha1_to_share, sf->sha1_digest);
		sf->flags &= ~SHARE_F_HAS_DIGEST;
		return FALSE;
	}

	/*
	 * If file was modified since the last time we computed the SHA1,
	 * recompute it and tell them that the SHA1 we have might not be
	 * accurate.
	 */

	if (
			sf->mtime != buf.st_mtime ||
			sf->file_size != (filesize_t) buf.st_size
	) {
		g_warning("shared file #%d \"%s\" changed, recomputing SHA1",
			sf->file_index, sf->file_path);
		sf->flags |= SHARE_F_RECOMPUTING;
		sf->mtime = buf.st_mtime;
		sf->file_size = buf.st_size;
		request_sha1(sf);
		return FALSE;
	}

	return TRUE;
}

void
shared_file_remove(struct shared_file *sf)
{
	const struct shared_file *sfc;
	
	g_assert(sf);

	sfc = shared_file(sf->file_index);
	if (SHARE_REBUILDING != sfc) {
		g_assert(sfc == sf);
		file_table[sf->file_index - 1] = NULL;
	}
	g_hash_table_remove(file_basenames, sf->name_nfc);
	if (0 == sf->refcnt) {
		shared_file_free(sf);
	}
}

/**
 * @returns the shared_file if we share a complete file bearing the given SHA1.
 * @returns NULL if we don't share a complete file, or SHARE_REBUILDING if the
 * set of shared file is being rebuilt.
 */
static struct shared_file *
shared_file_complete_by_sha1(gchar *sha1_digest)
{
	struct shared_file *f;

	if (sha1_to_share == NULL)			/* Not even begun share_scan() yet */
		return SHARE_REBUILDING;

	f = g_tree_lookup(sha1_to_share, sha1_digest);

	if (!f || !sha1_hash_available(f)) {
		/*
		 * If we're rebuilding the library, we might not have parsed the
		 * file yet, so it's possible we have this URN but we don't know
		 * it yet.	--RAM, 12/10/2002.
		 */

		if (file_table == NULL)			/* Rebuilding the library! */
			return SHARE_REBUILDING;

		return NULL;
	}

	return f;
}

/**
 * Take a given binary SHA1 digest, and return the corresponding
 * shared_file if we have it.
 *
 * @attention
 * NB: if the returned "shared_file" structure holds a non-NULL `fi',
 * then it means it is a partially shared file.
 */
shared_file_t *
shared_file_by_sha1(gchar *sha1_digest)
{
	struct shared_file *f;

	f = shared_file_complete_by_sha1(sha1_digest);

	/*
	 * If we don't share this file, or if we're rebuilding, and provided
	 * PFSP-server is enabled, look whether we don't have a partially
	 * downloaded file with this SHA1.
	 */

	if ((f == NULL || f == SHARE_REBUILDING) && pfsp_server) {
		struct shared_file *pf = file_info_shared_sha1(sha1_digest);
		if (pf != NULL)
			f = pf;
	}

	return f;
}

/**
 * Get accessor for ``kbytes_scanned''
 */
guint64
shared_kbytes_scanned(void)
{
	return kbytes_scanned;
}

/**
 * Get accessor for ``files_scanned''
 */
guint64
shared_files_scanned(void)
{
	return files_scanned;
}

/* vi: set ts=4 sw=4 cindent: */
