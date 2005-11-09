/*
 * $Id$
 *
 * Copyright (c) 2001-2003, Raphael Manfredi
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
 * @ingroup lib
 * @file
 *
 * Header parsing routines.
 *
 * @author Raphael Manfredi
 * @date 2001-2003
 */

#ifndef _header_h_
#define _header_h_

#include <stdio.h>
#include <glib.h>

/**
 * Header parsing and holding data structures.
 *
 * The `headers' field is a hash table indexed by field name (normalized).
 * Each value (GString) holds a private copy of the string making that header,
 * with all continuations removed (leading spaces collapsed into one), and
 * indentical fields concatenated using ", " separators, per RFC2616.
 *
 * The `fields' field holds a list of all the fields, in the order they
 * appeared.  The value is a header_field_t structure.  It allows one to
 * dump the header exactly as it was read.
 */

typedef struct header {
	GHashTable *headers;		/**< Indexed by name, normalized */
	GSList *fields;				/**< Ordered list of header_field_t */
	gint flags;					/**< Various operating flags */
	gint size;					/**< Total header size, in bytes */
	gint lines;					/**< Total header lines seen */
} header_t;

#define HEADER_SIZE(h)		((h)->size)
#define HEADER_LINES(h)		((h)->lines)

/**
 * A header field.
 *
 * It holds the field name, and all the lines that make up that field.
 * The first line has the field name and the ":" stripped, as well as
 * all the leading spaces.  Continuations also have their leading spaces
 * stripped out.
 *
 * For instance, assume the following header field:
 *
 *    - X-Comment: first line
 *         and continuation of first line
 *
 * Then the structure would contain, with () denoting a list:
 *
 *    - name = "X-Comment"
 *    - lines = ("first line", "and continuation of first line")
 */

typedef struct {
	gchar *name;				/**< Field name */
	GSList *lines;				/**< List of lines making this header */
} header_field_t;

/*
 * Error codes.
 */

#define HEAD_OK				0		/**< OK */
#define HEAD_CONTINUATION	1		/**< Unexpected continuation line */
#define HEAD_MALFORMED		2		/**< Malformed header line */
#define HEAD_BAD_CHARS		3		/**< Invalid characters in field name */
#define HEAD_EOH_REACHED	4		/**< End of header already reached */
#define HEAD_SKIPPED		5		/**< Skipped continuation line */
#define HEAD_TOO_LARGE		6		/**< Header too large */
#define HEAD_MANY_LINES		7		/**< Header has too many lines */
#define HEAD_EOH			8		/**< End of header reached */

/*
 * Our sanity limits
 */

#define HEAD_MAX_LINES		128		/**< Maximum amount of header lines */
#define HEAD_MAX_SIZE		16384	/**< Maximum size of header data */

/*
 * Public interface.
 */

header_t *header_make(void);
void header_free(header_t *o);
void header_reset(header_t *o);
gint header_append(header_t *o, const gchar *text, gint len);
void header_dump(const header_t *o, FILE *out);
const gchar *header_strerror(guint errnum);
gchar *header_get(const header_t *o, const gchar *field);
gchar *header_getdup(const header_t *o, const gchar *field);

gpointer header_fmt_make(const gchar *field, const gchar *separator,
	gint len_hint);
void header_fmt_free(gpointer o);
void header_fmt_set_line_length(gpointer o, gint maxlen);
gboolean header_fmt_value_fits(gpointer o, gint len, gint maxlen);
void header_fmt_append(gpointer o, const gchar *str, const gchar *separator);
void header_fmt_append_value(gpointer o, const gchar *str);
gint header_fmt_length(gpointer o);
void header_fmt_end(gpointer o);
gchar *header_fmt_string(gpointer o);
gchar *header_fmt_to_string(gpointer o);

#endif	/* _header_h_ */

/* vi: set ts=4 sw=4 cindent: */
